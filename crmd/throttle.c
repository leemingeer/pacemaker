/*
 * Copyright 2013-2018 Andrew Beekhof <andrew@beekhof.net>
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <unistd.h>
#include <ctype.h>
#include <dirent.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/cluster.h>

#include <crmd_fsa.h>
#include <throttle.h>


enum throttle_state_e {
    throttle_extreme = 0x1000,
    throttle_high = 0x0100,
    throttle_med  = 0x0010,
    throttle_low  = 0x0001,
    throttle_none = 0x0000,
};

struct throttle_record_s {
    int max;
    enum throttle_state_e mode;
    char *node;
};

static int throttle_job_max = 0;
static float throttle_load_target = 0.0;

#define THROTTLE_FACTOR_LOW    1.2
#define THROTTLE_FACTOR_MEDIUM 1.6
#define THROTTLE_FACTOR_HIGH   2.0

static GHashTable *throttle_records = NULL;
static mainloop_timer_t *throttle_timer = NULL;

#if SUPPORT_PROCFS
/*!
 * \internal
 * \brief Return name of /proc file containing the CIB deamon's load statistics
 *
 * \return Newly allocated memory with file name on success, NULL otherwise
 *
 * \note It is the caller's responsibility to free the return value.
 *       This will return NULL if the daemon is being run via valgrind.
 *       This should be called only on Linux systems.
 */
static char *
find_cib_loadfile(void)
{
    int pid = crm_procfs_pid_of("cib");

    return pid? crm_strdup_printf("/proc/%d/stat", pid) : NULL;
}

static bool
throttle_cib_load(float *load)
{
/*
       /proc/[pid]/stat
              Status information about the process.  This is used by ps(1).  It is defined in /usr/src/linux/fs/proc/array.c.

              The fields, in order, with their proper scanf(3) format specifiers, are:

              pid %d      (1) The process ID.

              comm %s     (2) The filename of the executable, in parentheses.  This is visible whether or not the executable is swapped out.

              state %c    (3) One character from the string "RSDZTW" where R is running, S is sleeping in an interruptible wait, D is waiting in uninterruptible disk sleep, Z is zombie, T is traced or stopped (on a signal), and W is paging.

              ppid %d     (4) The PID of the parent.

              pgrp %d     (5) The process group ID of the process.

              session %d  (6) The session ID of the process.

              tty_nr %d   (7) The controlling terminal of the process.  (The minor device number is contained in the combination of bits 31 to 20 and 7 to 0; the major device number is in bits 15 to 8.)

              tpgid %d    (8) The ID of the foreground process group of the controlling terminal of the process.

              flags %u (%lu before Linux 2.6.22)
                          (9) The kernel flags word of the process.  For bit meanings, see the PF_* defines in the Linux kernel source file include/linux/sched.h.  Details depend on the kernel version.

              minflt %lu  (10) The number of minor faults the process has made which have not required loading a memory page from disk.

              cminflt %lu (11) The number of minor faults that the process's waited-for children have made.

              majflt %lu  (12) The number of major faults the process has made which have required loading a memory page from disk.

              cmajflt %lu (13) The number of major faults that the process's waited-for children have made.

              utime %lu   (14) Amount of time that this process has been scheduled in user mode, measured in clock ticks (divide by sysconf(_SC_CLK_TCK)).  This includes guest time, guest_time (time spent running a virtual CPU, see below), so that applications that are not aware of the guest time field do not lose that time from their calculations.

              stime %lu   (15) Amount of time that this process has been scheduled in kernel mode, measured in clock ticks (divide by sysconf(_SC_CLK_TCK)).
 */

    static char *loadfile = NULL;
    static time_t last_call = 0;
    static long ticks_per_s = 0;
    static unsigned long last_utime, last_stime;

    char buffer[64*1024];
    FILE *stream = NULL;
    time_t now = time(NULL);

    if(load == NULL) {
        return FALSE;
    } else {
        *load = 0.0;
    }

    if(loadfile == NULL) {
        last_call = 0;
        last_utime = 0;
        last_stime = 0;
        loadfile = find_cib_loadfile();
        if (loadfile == NULL) {
            crm_warn("Couldn't find CIB load file");
            return FALSE;
        }
        ticks_per_s = sysconf(_SC_CLK_TCK);
        crm_trace("Found %s", loadfile);
    }

    stream = fopen(loadfile, "r");
    if(stream == NULL) {
        int rc = errno;

        crm_warn("Couldn't read %s: %s (%d)", loadfile, pcmk_strerror(rc), rc);
        free(loadfile); loadfile = NULL;
        return FALSE;
    }

    if(fgets(buffer, sizeof(buffer), stream)) {
        char *comm = calloc(1, 256);
        char state = 0;
        int rc = 0, pid = 0, ppid = 0, pgrp = 0, session = 0, tty_nr = 0, tpgid = 0;
        unsigned long flags = 0, minflt = 0, cminflt = 0, majflt = 0, cmajflt = 0, utime = 0, stime = 0;

        rc = sscanf(buffer,  "%d %[^ ] %c %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu",
                    &pid, comm, &state,
                    &ppid, &pgrp, &session, &tty_nr, &tpgid,
                    &flags, &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime);
        free(comm);

        if(rc != 15) {
            crm_err("Only %d of 15 fields found in %s", rc, loadfile);
            fclose(stream);
            return FALSE;

        } else if(last_call > 0
           && last_call < now
           && last_utime <= utime
           && last_stime <= stime) {

            time_t elapsed = now - last_call;
            unsigned long delta_utime = utime - last_utime;
            unsigned long delta_stime = stime - last_stime;

            *load = (delta_utime + delta_stime); /* Cast to a float before division */
            *load /= ticks_per_s;
            *load /= elapsed;
            crm_debug("cib load: %f (%lu ticks in %lds)", *load, delta_utime + delta_stime, (long)elapsed);

        } else {
            crm_debug("Init %lu + %lu ticks at %ld (%lu tps)", utime, stime, (long)now, ticks_per_s);
        }

        last_call = now;
        last_utime = utime;
        last_stime = stime;

        fclose(stream);
        return TRUE;
    }

    fclose(stream);
    return FALSE;
}

static bool
throttle_load_avg(float *load)
{
    char buffer[256];
    FILE *stream = NULL;
    const char *loadfile = "/proc/loadavg";

    if(load == NULL) {
        return FALSE;
    }

    stream = fopen(loadfile, "r");
    if(stream == NULL) {
        int rc = errno;
        crm_warn("Couldn't read %s: %s (%d)", loadfile, pcmk_strerror(rc), rc);
        return FALSE;
    }

    if(fgets(buffer, sizeof(buffer), stream)) {
        char *nl = strstr(buffer, "\n");

        /* Grab the 1-minute average, ignore the rest */
        *load = strtof(buffer, NULL);
        if(nl) { nl[0] = 0; }

        fclose(stream);
        return TRUE;
    }

    fclose(stream);
    return FALSE;
}

/*!
 * \internal
 * \brief Check a load value against throttling thresholds
 *
 * \param[in] load        Load value to check
 * \param[in] desc        Description of metric (for logging)
 * \param[in] thresholds  Low/medium/high/extreme thresholds
 *
 * \return Throttle mode corresponding to load value
 */
static enum throttle_state_e
throttle_check_thresholds(float load, const char *desc, float thresholds[4])
{
    if (load > thresholds[3]) {
        crm_notice("Extreme %s detected: %f", desc, load);
        return throttle_extreme;

    } else if (load > thresholds[2]) {
        crm_notice("High %s detected: %f", desc, load);
        return throttle_high;

    } else if (load > thresholds[1]) {
        crm_info("Moderate %s detected: %f", desc, load);
        return throttle_med;

    } else if (load > thresholds[0]) {
        crm_debug("Noticeable %s detected: %f", desc, load);
        return throttle_low;
    }

    crm_trace("Negligible %s detected: %f", desc, load);
    return throttle_none;
}

static enum throttle_state_e
throttle_handle_load(float load, const char *desc, int cores)
{
    float normalize;
    float thresholds[4];

    if (cores == 1) {
        /* On a single core machine, a load of 1.0 is already too high */
        normalize = 0.6;

    } else {
        /* Normalize the load to be per-core */
        normalize = cores;
    }
    thresholds[0] = throttle_load_target * normalize * THROTTLE_FACTOR_LOW;
    thresholds[1] = throttle_load_target * normalize * THROTTLE_FACTOR_MEDIUM;
    thresholds[2] = throttle_load_target * normalize * THROTTLE_FACTOR_HIGH;
    thresholds[3] = load + 1.0; /* never extreme */

    return throttle_check_thresholds(load, desc, thresholds);
}
#endif

static enum throttle_state_e
throttle_mode(void)
{
#if SUPPORT_PROCFS
    unsigned int cores;
    float load;
    float thresholds[4];
    enum throttle_state_e mode = throttle_none;

    cores = crm_procfs_num_cores();
    if(throttle_cib_load(&load)) {
        float cib_max_cpu = 0.95;

        /* The CIB is a single-threaded task and thus cannot consume
         * more than 100% of a CPU (and 1/cores of the overall system
         * load).
         *
         * On a many-cored system, the CIB might therefore be maxed out
         * (causing operations to fail or appear to fail) even though
         * the overall system load is still reasonable.
         *
         * Therefore, the 'normal' thresholds can not apply here, and we
         * need a special case.
         */
        if(cores == 1) {
            cib_max_cpu = 0.4;
        }
        if(throttle_load_target > 0.0 && throttle_load_target < cib_max_cpu) {
            cib_max_cpu = throttle_load_target;
        }

        thresholds[0] = cib_max_cpu * 0.8;
        thresholds[1] = cib_max_cpu * 0.9;
        thresholds[2] = cib_max_cpu;
        /* Can only happen on machines with a low number of cores */
        thresholds[3] = cib_max_cpu * 1.5;

        mode |= throttle_check_thresholds(load, "CIB load", thresholds);
    }

    if(throttle_load_target <= 0) {
        /* If we ever make this a valid value, the cluster will at least behave as expected */
        return mode;
    }

    if(throttle_load_avg(&load)) {
        crm_debug("Current load is %f across %u core(s)", load, cores);
        mode |= throttle_handle_load(load, "CPU load", cores);
    }

    if(mode & throttle_extreme) {
        return throttle_extreme;
    } else if(mode & throttle_high) {
        return throttle_high;
    } else if(mode & throttle_med) {
        return throttle_med;
    } else if(mode & throttle_low) {
        return throttle_low;
    }
#endif // SUPPORT_PROCFS
    return throttle_none;
}

static void
throttle_send_command(enum throttle_state_e mode)
{
    xmlNode *xml = NULL;
    static enum throttle_state_e last = -1;

    if(mode != last) {
        crm_info("New throttle mode: %.4x (was %.4x)", mode, last);
        last = mode;

        xml = create_request(CRM_OP_THROTTLE, NULL, NULL, CRM_SYSTEM_CRMD, CRM_SYSTEM_CRMD, NULL);
        crm_xml_add_int(xml, F_CRM_THROTTLE_MODE, mode);
        crm_xml_add_int(xml, F_CRM_THROTTLE_MAX, throttle_job_max);

        send_cluster_message(NULL, crm_msg_crmd, xml, TRUE);
        free_xml(xml);
    }
}

static gboolean
throttle_timer_cb(gpointer data)
{
    static bool send_updates = FALSE;
    enum throttle_state_e now = throttle_none;

    if(send_updates) {
        now = throttle_mode();
        throttle_send_command(now);

    } else if(compare_version(fsa_our_dc_version, "3.0.8") < 0) {
        /* Optimize for the true case */
        crm_trace("DC version %s doesn't support throttling", fsa_our_dc_version);

    } else {
        send_updates = TRUE;
        now = throttle_mode();
        throttle_send_command(now);
    }

    return TRUE;
}

static void
throttle_record_free(gpointer p)
{
    struct throttle_record_s *r = p;
    free(r->node);
    free(r);
}

void
throttle_set_load_target(float target)
{
    throttle_load_target = target;
}

void
throttle_update_job_max(const char *preference)
{
    int max = 0;

    throttle_job_max = 2 * crm_procfs_num_cores();

    if(preference) {
        /* Global preference from the CIB */
        max = crm_int_helper(preference, NULL);
        if(max > 0) {
            throttle_job_max = max;
        }
    }

    preference = getenv("LRMD_MAX_CHILDREN");
    if(preference) {
        /* Legacy env variable */
        max = crm_int_helper(preference, NULL);
        if(max > 0) {
            throttle_job_max = max;
        }
    }

    preference = getenv("PCMK_node_action_limit");
    if(preference) {
        /* Per-node override */
        max = crm_int_helper(preference, NULL);
        if(max > 0) {
            throttle_job_max = max;
        }
    }
}

void
throttle_init(void)
{
    if(throttle_records == NULL) {
        throttle_records = g_hash_table_new_full(
            crm_str_hash, g_str_equal, NULL, throttle_record_free);
        throttle_timer = mainloop_timer_add("throttle", 30 * 1000, TRUE, throttle_timer_cb, NULL);
    }

    throttle_update_job_max(NULL);
    mainloop_timer_start(throttle_timer);
}

void
throttle_fini(void)
{
    mainloop_timer_del(throttle_timer); throttle_timer = NULL;
    g_hash_table_destroy(throttle_records); throttle_records = NULL;
}

int
throttle_get_total_job_limit(int l)
{
    /* Cluster-wide limit */
    GHashTableIter iter;
    int limit = l;
    int peers = crm_active_peers();
    struct throttle_record_s *r = NULL;

    g_hash_table_iter_init(&iter, throttle_records);

    while (g_hash_table_iter_next(&iter, NULL, (gpointer *) &r)) {
        switch(r->mode) {

            case throttle_extreme:
                if(limit == 0 || limit > peers/4) {
                    limit = QB_MAX(1, peers/4);
                }
                break;

            case throttle_high:
                if(limit == 0 || limit > peers/2) {
                    limit = QB_MAX(1, peers/2);
                }
                break;
            default:
                break;
        }
    }
    if(limit == l) {
        /* crm_trace("No change to batch-limit=%d", limit); */

    } else if(l == 0) {
        crm_trace("Using batch-limit=%d", limit);

    } else {
        crm_trace("Using batch-limit=%d instead of %d", limit, l);
    }
    return limit;
}

int
throttle_get_job_limit(const char *node)
{
    int jobs = 1;
    struct throttle_record_s *r = NULL;

    r = g_hash_table_lookup(throttle_records, node);
    if(r == NULL) {
        r = calloc(1, sizeof(struct throttle_record_s));
        r->node = strdup(node);
        r->mode = throttle_low;
        r->max = throttle_job_max;
        crm_trace("Defaulting to local values for unknown node %s", node);

        g_hash_table_insert(throttle_records, r->node, r);
    }

    switch(r->mode) {
        case throttle_extreme:
        case throttle_high:
            jobs = 1; /* At least one job must always be allowed */
            break;
        case throttle_med:
            jobs = QB_MAX(1, r->max / 4);
            break;
        case throttle_low:
            jobs = QB_MAX(1, r->max / 2);
            break;
        case throttle_none:
            jobs = QB_MAX(1, r->max);
            break;
        default:
            crm_err("Unknown throttle mode %.4x on %s", r->mode, node);
            break;
    }
    return jobs;
}

void
throttle_update(xmlNode *xml)
{
    int max = 0;
    enum throttle_state_e mode = 0;
    struct throttle_record_s *r = NULL;
    const char *from = crm_element_value(xml, F_CRM_HOST_FROM);

    crm_element_value_int(xml, F_CRM_THROTTLE_MODE, (int*)&mode);
    crm_element_value_int(xml, F_CRM_THROTTLE_MAX, &max);

    r = g_hash_table_lookup(throttle_records, from);

    if(r == NULL) {
        r = calloc(1, sizeof(struct throttle_record_s));
        r->node = strdup(from);
        g_hash_table_insert(throttle_records, r->node, r);
    }

    r->max = max;
    r->mode = mode;

    crm_debug("Host %s supports a maximum of %d jobs and throttle mode %.4x.  New job limit is %d",
              from, max, mode, throttle_get_job_limit(from));
}
