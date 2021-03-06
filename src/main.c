#include "common.h"

extern void *cgroups_main(void *ptr);

void netdata_cleanup_and_exit(int ret) {
    netdata_exit = 1;

    error_log_limit_unlimited();

    debug(D_EXIT, "Called: netdata_cleanup_and_exit()");

    // save the database
    rrdset_save_all();

    // unlink the pid
    if(pidfile[0]) {
        if(unlink(pidfile) != 0)
            error("Cannot unlink pidfile '%s'.", pidfile);
    }

#ifdef NETDATA_INTERNAL_CHECKS
    // kill all childs
    //kill_childs();

    // free database
    rrdset_free_all();
#endif

    info("netdata exiting. Bye bye...");
    exit(ret);
}

struct netdata_static_thread static_threads[] = {
#ifdef INTERNAL_PLUGIN_NFACCT
// nfacct requires root access
    // so, we build it as an external plugin with setuid to root
    {"nfacct",              "plugins",  "nfacct",     1, NULL, NULL, nfacct_main},
#endif

    {"tc",                 "plugins",   "tc",         1, NULL, NULL, tc_main},
    {"idlejitter",         "plugins",   "idlejitter", 1, NULL, NULL, cpuidlejitter_main},
#if defined(__FreeBSD__)
    {"freebsd",            "plugins",   "freebsd",    1, NULL, NULL, freebsd_main},
#elif defined(__APPLE__)
    {"macos",              "plugins",   "macos",      1, NULL, NULL, macos_main},
#else
    {"proc",               "plugins",   "proc",       1, NULL, NULL, proc_main},
    {"diskspace",          "plugins",   "diskspace",  1, NULL, NULL, proc_diskspace_main},
    {"cgroups",            "plugins",   "cgroups",    1, NULL, NULL, cgroups_main},
#endif /* __FreeBSD__, __APPLE__*/
    {"check",              "plugins",   "checks",     0, NULL, NULL, checks_main},
    {"backends",            NULL,       NULL,         1, NULL, NULL, backends_main},
    {"health",              NULL,       NULL,         1, NULL, NULL, health_main},
    {"plugins.d",           NULL,       NULL,         1, NULL, NULL, pluginsd_main},
    {"web",                 NULL,       NULL,         1, NULL, NULL, socket_listen_main_multi_threaded},
    {"web-single-threaded", NULL,       NULL,         0, NULL, NULL, socket_listen_main_single_threaded},
    {NULL,                  NULL,       NULL,         0, NULL, NULL, NULL}
};

void web_server_threading_selection(void) {
    int threaded = config_get_boolean("global", "multi threaded web server", 1);

    int i;
    for(i = 0; static_threads[i].name ; i++) {
        if(static_threads[i].start_routine == socket_listen_main_multi_threaded)
            static_threads[i].enabled = threaded?1:0;

        if(static_threads[i].start_routine == socket_listen_main_single_threaded)
            static_threads[i].enabled = threaded?0:1;
    }

    web_client_timeout = (int) config_get_number("global", "disconnect idle web clients after seconds", DEFAULT_DISCONNECT_IDLE_WEB_CLIENTS_AFTER_SECONDS);

    web_donotrack_comply = config_get_boolean("global", "respect web browser do not track policy", web_donotrack_comply);

#ifdef NETDATA_WITH_ZLIB
    web_enable_gzip = config_get_boolean("global", "enable web responses gzip compression", web_enable_gzip);

    char *s = config_get("global", "web compression strategy", "default");
    if(!strcmp(s, "default"))
        web_gzip_strategy = Z_DEFAULT_STRATEGY;
    else if(!strcmp(s, "filtered"))
        web_gzip_strategy = Z_FILTERED;
    else if(!strcmp(s, "huffman only"))
        web_gzip_strategy = Z_HUFFMAN_ONLY;
    else if(!strcmp(s, "rle"))
        web_gzip_strategy = Z_RLE;
    else if(!strcmp(s, "fixed"))
        web_gzip_strategy = Z_FIXED;
    else {
        error("Invalid compression strategy '%s'. Valid strategies are 'default', 'filtered', 'huffman only', 'rle' and 'fixed'. Proceeding with 'default'.", s);
        web_gzip_strategy = Z_DEFAULT_STRATEGY;
    }

    web_gzip_level = (int)config_get_number("global", "web compression level", 3);
    if(web_gzip_level < 1) {
        error("Invalid compression level %d. Valid levels are 1 (fastest) to 9 (best ratio). Proceeding with level 1 (fastest compression).", web_gzip_level);
        web_gzip_level = 1;
    }
    else if(web_gzip_level > 9) {
        error("Invalid compression level %d. Valid levels are 1 (fastest) to 9 (best ratio). Proceeding with level 9 (best compression).", web_gzip_level);
        web_gzip_level = 9;
    }
#endif /* NETDATA_WITH_ZLIB */
}


int killpid(pid_t pid, int sig)
{
    int ret = -1;
    debug(D_EXIT, "Request to kill pid %d", pid);

    errno = 0;
    if(kill(pid, 0) == -1) {
        switch(errno) {
            case ESRCH:
                error("Request to kill pid %d, but it is not running.", pid);
                break;

            case EPERM:
                error("Request to kill pid %d, but I do not have enough permissions.", pid);
                break;

            default:
                error("Request to kill pid %d, but I received an error.", pid);
                break;
        }
    }
    else {
        errno = 0;
        ret = kill(pid, sig);
        if(ret == -1) {
            switch(errno) {
                case ESRCH:
                    error("Cannot kill pid %d, but it is not running.", pid);
                    break;

                case EPERM:
                    error("Cannot kill pid %d, but I do not have enough permissions.", pid);
                    break;

                default:
                    error("Cannot kill pid %d, but I received an error.", pid);
                    break;
            }
        }
    }

    return ret;
}

void kill_childs()
{
    error_log_limit_unlimited();

    siginfo_t info;

    struct web_client *w;
    for(w = web_clients; w ; w = w->next) {
        info("Stopping web client %s", w->client_ip);
        pthread_cancel(w->thread);
        // it is detached
        // pthread_join(w->thread, NULL);

        w->obsolete = 1;
    }

    int i;
    for (i = 0; static_threads[i].name != NULL ; i++) {
        if(static_threads[i].enabled) {
            info("Stopping %s thread", static_threads[i].name);
            pthread_cancel(*static_threads[i].thread);
            // it is detached
            // pthread_join(*static_threads[i].thread, NULL);

            static_threads[i].enabled = 0;
        }
    }

    if(tc_child_pid) {
        info("Killing tc-qos-helper process %d", tc_child_pid);
        if(killpid(tc_child_pid, SIGTERM) != -1)
            waitid(P_PID, (id_t) tc_child_pid, &info, WEXITED);

        tc_child_pid = 0;
    }

    struct plugind *cd;
    for(cd = pluginsd_root ; cd ; cd = cd->next) {
        if(cd->enabled && !cd->obsolete) {
            info("Stopping %s plugin thread", cd->id);
            pthread_cancel(cd->thread);

            if(cd->pid) {
                info("killing %s plugin child process pid %d", cd->id, cd->pid);
                if(killpid(cd->pid, SIGTERM) != -1)
                    waitid(P_PID, (id_t) cd->pid, &info, WEXITED);

                cd->pid = 0;
            }

            cd->obsolete = 1;
        }
    }

    // if, for any reason there is any child exited
    // catch it here
    info("Cleaning up an other children");
    waitid(P_PID, 0, &info, WEXITED|WNOHANG);

    info("All threads/childs stopped.");
}

struct option_def options[] = {
    // opt description                                    arg name       default value
    { 'c', "Configuration file to load.",                 "filename",    CONFIG_DIR "/" CONFIG_FILENAME},
    { 'D', "Do not fork. Run in the foreground.",         NULL,          "run in the background"},
    { 'h', "Display this help message.",                  NULL,          NULL},
    { 'P', "File to save a pid while running.",           "filename",    "do not save pid to a file"},
    { 'i', "The IP address to listen to.",                "IP",          "all IP addresses IPv4 and IPv6"},
    { 'k', "Check health configuration and exit.",        NULL,          NULL},
    { 'p', "API/Web port to use.",                        "port",        "19999"},
    { 's', "Prefix for /proc and /sys (for containers).", "path",        "no prefix"},
    { 't', "The internal clock of netdata.",              "seconds",     "1"},
    { 'u', "Run as user.",                                "username",    "netdata"},
    { 'v', "Print netdata version and exit.",             NULL,          NULL},
    { 'W', "See Advanced options below.",                 "options",     NULL},
};

void help(int exitcode) {
    FILE *stream;
    if(exitcode == 0)
        stream = stdout;
    else
        stream = stderr;

    int num_opts = sizeof(options) / sizeof(struct option_def);
    int i;
    int max_len_arg = 0;

    // Compute maximum argument length
    for( i = 0; i < num_opts; i++ ) {
        if(options[i].arg_name) {
            int len_arg = (int)strlen(options[i].arg_name);
            if(len_arg > max_len_arg) max_len_arg = len_arg;
        }
    }

    if(max_len_arg > 30) max_len_arg = 30;
    if(max_len_arg < 20) max_len_arg = 20;

    fprintf(stream, "%s", "\n"
            " ^\n"
            " |.-.   .-.   .-.   .-.   .  netdata                                         \n"
            " |   '-'   '-'   '-'   '-'   real-time performance monitoring, done right!   \n"
            " +----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+--->\n"
            "\n"
            " Copyright (C) 2016-2017, Costa Tsaousis <costa@tsaousis.gr>\n"
            " Released under GNU General Public License v3 or later.\n"
            " All rights reserved.\n"
            "\n"
            " Home Page  : https://my-netdata.io\n"
            " Source Code: https://github.com/firehol/netdata\n"
            " Wiki / Docs: https://github.com/firehol/netdata/wiki\n"
            " Support    : https://github.com/firehol/netdata/issues\n"
            " License    : https://github.com/firehol/netdata/blob/master/LICENSE.md\n"
            "\n"
            " Twitter    : https://twitter.com/linuxnetdata\n"
            " Facebook   : https://www.facebook.com/linuxnetdata/\n"
            "\n"
            " netdata is a https://firehol.org project.\n"
            "\n"
            "\n"
    );

    fprintf(stream, " SYNOPSIS: netdata [options]\n");
    fprintf(stream, "\n");
    fprintf(stream, " Options:\n\n");

    // Output options description.
    for( i = 0; i < num_opts; i++ ) {
        fprintf(stream, "  -%c %-*s  %s", options[i].val, max_len_arg, options[i].arg_name ? options[i].arg_name : "", options[i].description);
        if(options[i].default_value) {
            fprintf(stream, "\n   %c %-*s  Default: %s\n", ' ', max_len_arg, "", options[i].default_value);
        } else {
            fprintf(stream, "\n");
        }
        fprintf(stream, "\n");
    }

    fprintf(stream, "\n Advanced options:\n\n"
            "  -W stacksize=N           Set the stacksize (in bytes).\n\n"
            "  -W debug_flags=N         Set runtime tracing to debug.log.\n\n"
            "  -W unittest              Run internal unittests and exit.\n\n"
            "  -W simple-pattern pattern string\n"
            "                           Check if string matches pattern and exit.\n\n"
    );

    fprintf(stream, "\n Signals netdata handles:\n\n"
            "  - HUP                    Close and reopen log files.\n"
            "  - USR1                   Save internal DB to disk.\n"
            "  - USR2                   Reload health configuration.\n"
            "\n"
    );

    fflush(stream);
    exit(exitcode);
}

// TODO: Remove this function with the nix major release.
void remove_option(int opt_index, int *argc, char **argv) {
    int i = opt_index;
    // remove the options.
    do {
        *argc = *argc - 1;
        for(i = opt_index; i < *argc; i++) {
            argv[i] = argv[i+1];
        }
        i = opt_index;
    } while(argv[i][0] != '-' && opt_index >= *argc);
}

static const char *verify_required_directory(const char *dir) {
    if(chdir(dir) == -1)
        fatal("Cannot cd to directory '%s'", dir);

    DIR *d = opendir(dir);
    if(!d)
        fatal("Cannot examine the contents of directory '%s'", dir);
    closedir(d);

    return dir;
}

int main(int argc, char **argv) {
    char *hostname = "localhost";
    int i, check_config = 0;
    int config_loaded = 0;
    int dont_fork = 0;
    size_t wanted_stacksize = 0, stacksize = 0;
    pthread_attr_t attr;

    // set the name for logging
    program_name = "netdata";

    // parse depercated options
    // TODO: Remove this block with the next major release.
    {
        i = 1;
        while(i < argc) {
            if(strcmp(argv[i], "-pidfile") == 0 && (i+1) < argc) {
                strncpyz(pidfile, argv[i+1], FILENAME_MAX);
                fprintf(stderr, "%s: deprecated option -- %s -- please use -P instead.\n", argv[0], argv[i]);
                remove_option(i, &argc, argv);
            }
            else if(strcmp(argv[i], "-nodaemon") == 0 || strcmp(argv[i], "-nd") == 0) {
                dont_fork = 1;
                fprintf(stderr, "%s: deprecated option -- %s -- please use -D instead.\n ", argv[0], argv[i]);
                remove_option(i, &argc, argv);
            }
            else if(strcmp(argv[i], "-ch") == 0 && (i+1) < argc) {
                config_set("global", "host access prefix", argv[i+1]);
                fprintf(stderr, "%s: deprecated option -- %s -- please use -s instead.\n", argv[0], argv[i]);
                remove_option(i, &argc, argv);
            }
            else if(strcmp(argv[i], "-l") == 0 && (i+1) < argc) {
                config_set("global", "history", argv[i+1]);
                fprintf(stderr, "%s: deprecated option -- %s -- This option will be removed with V2.*.\n", argv[0], argv[i]);
                remove_option(i, &argc, argv);
            }
            else i++;
        }
    }

    // parse options
    {
        int num_opts = sizeof(options) / sizeof(struct option_def);
        char optstring[(num_opts * 2) + 1];

        int string_i = 0;
        for( i = 0; i < num_opts; i++ ) {
            optstring[string_i] = options[i].val;
            string_i++;
            if(options[i].arg_name) {
                optstring[string_i] = ':';
                string_i++;
            }
        }
        // terminate optstring
        optstring[string_i] ='\0';
        optstring[(num_opts *2)] ='\0';

        int opt;
        while( (opt = getopt(argc, argv, optstring)) != -1 ) {
            switch(opt) {
                case 'c':
                    if(load_config(optarg, 1) != 1) {
                        error("Cannot load configuration file %s.", optarg);
                        exit(1);
                    }
                    else {
                        debug(D_OPTIONS, "Configuration loaded from %s.", optarg);
                        config_loaded = 1;
                    }
                    break;
                case 'D':
                    dont_fork = 1;
                    break;
                case 'h':
                    help(0);
                    break;
                case 'i':
                    config_set("global", "bind to", optarg);
                    break;
                case 'k':
                    dont_fork = 1;
                    check_config = 1;
                    break;
                case 'P':
                    strncpy(pidfile, optarg, FILENAME_MAX);
                    pidfile[FILENAME_MAX] = '\0';
                    break;
                case 'p':
                    config_set("global", "default port", optarg);
                    break;
                case 's':
                    config_set("global", "host access prefix", optarg);
                    break;
                case 't':
                    config_set("global", "update every", optarg);
                    break;
                case 'u':
                    config_set("global", "run as user", optarg);
                    break;
                case 'v':
                    printf("%s %s\n", program_name, program_version);
                    return 0;
                case 'W':
                    {
                        char* stacksize_string = "stacksize=";
                        char* debug_flags_string = "debug_flags=";
                        if(strcmp(optarg, "unittest") == 0) {
                            rrd_update_every = 1;
                            if(run_all_mockup_tests()) exit(1);
                            if(unit_test_storage()) exit(1);
                            fprintf(stderr, "\n\nALL TESTS PASSED\n\n");
                            exit(0);
                        }
                        else if(strcmp(optarg, "simple-pattern") == 0) {
                            if(optind + 2 > argc) {
                                fprintf(stderr, "%s", "\nUSAGE: -W simple-pattern 'pattern' 'string'\n\n"
                                        " Checks if 'pattern' matches the given 'string'.\n"
                                        " - 'pattern' can be one or more space separated words.\n"
                                        " - each 'word' can contain one or more asterisks.\n"
                                        " - words starting with '!' give negative matches.\n"
                                        " - words are processed left to right\n"
                                        "\n"
                                        "Examples:\n"
                                        "\n"
                                        " > match all veth interfaces, except veth0:\n"
                                        "\n"
                                        "   -W simple-pattern '!veth0 veth*' 'veth12'\n"
                                        "\n"
                                        "\n"
                                        " > match all *.ext files directly in /path/:\n"
                                        "   (this will not match *.ext files in a subdir of /path/)\n"
                                        "\n"
                                        "   -W simple-pattern '!/path/*/*.ext /path/*.ext' '/path/test.ext'\n"
                                        "\n"
                                );
                                exit(1);
                            }

                            const char *heystack = argv[optind];
                            const char *needle = argv[optind + 1];

                            SIMPLE_PATTERN *p = simple_pattern_create(heystack
                                                                      , SIMPLE_PATTERN_EXACT);
                            int ret = simple_pattern_matches(p, needle);
                            simple_pattern_free(p);

                            if(ret) {
                                fprintf(stdout, "RESULT: MATCHED - pattern '%s' matches '%s'\n", heystack, needle);
                                exit(0);
                            }
                            else {
                                fprintf(stdout, "RESULT: NOT MATCHED - pattern '%s' does not match '%s'\n", heystack, needle);
                                exit(1);
                            }
                        }
                        else if(strncmp(optarg, stacksize_string, strlen(stacksize_string)) == 0) {
                            optarg += strlen(stacksize_string);
                            config_set("global", "pthread stack size", optarg);
                        }
                        else if(strncmp(optarg, debug_flags_string, strlen(debug_flags_string)) == 0) {
                            optarg += strlen(debug_flags_string);
                            config_set("global", "debug flags",  optarg);
                            debug_flags = strtoull(optarg, NULL, 0);
                        }
                    }
                    break;
                default: /* ? */
                    help(1);
                    break;
            }
        }
    }

#ifdef _SC_OPEN_MAX
    // close all open file descriptors, except the standard ones
    // the caller may have left open files (lxc-attach has this issue)
    {
        int fd;
        for(fd = (int) (sysconf(_SC_OPEN_MAX) - 1); fd > 2; fd--)
            if(fd_is_valid(fd)) close(fd);
    }
#endif

    if(!config_loaded)
        load_config(NULL, 0);

    {
        char *pmax = config_get("global", "glibc malloc arena max for plugins", "1");
        if(pmax && *pmax)
            setenv("MALLOC_ARENA_MAX", pmax, 1);

#if defined(HAVE_C_MALLOPT)
        int i = config_get_number("global", "glibc malloc arena max for netdata", 1);
        if(i > 0)
            mallopt(M_ARENA_MAX, 1);
#endif

        // prepare configuration environment variables for the plugins

        netdata_configured_config_dir  = config_get("global", "config directory",    CONFIG_DIR);
        netdata_configured_log_dir     = config_get("global", "log directory",       LOG_DIR);
        netdata_configured_plugins_dir = config_get("global", "plugins directory",   PLUGINS_DIR);
        netdata_configured_web_dir     = config_get("global", "web files directory", WEB_DIR);
        netdata_configured_cache_dir   = config_get("global", "cache directory",     CACHE_DIR);
        netdata_configured_varlib_dir  = config_get("global", "lib directory",       VARLIB_DIR);
        netdata_configured_home_dir    = config_get("global", "home directory",      CACHE_DIR);

        setenv("NETDATA_CONFIG_DIR" , verify_required_directory(netdata_configured_config_dir),  1);
        setenv("NETDATA_PLUGINS_DIR", verify_required_directory(netdata_configured_plugins_dir), 1);
        setenv("NETDATA_WEB_DIR"    , verify_required_directory(netdata_configured_web_dir),     1);
        setenv("NETDATA_CACHE_DIR"  , verify_required_directory(netdata_configured_cache_dir),   1);
        setenv("NETDATA_LIB_DIR"    , verify_required_directory(netdata_configured_varlib_dir),  1);
        setenv("NETDATA_LOG_DIR"    , verify_required_directory(netdata_configured_log_dir),     1);
        setenv("HOME"               , verify_required_directory(netdata_configured_home_dir),    1);

        netdata_configured_host_prefix = config_get("global", "host access prefix", "");
        setenv("NETDATA_HOST_PREFIX", netdata_configured_host_prefix, 1);

        // disable buffering for python plugins
        setenv("PYTHONUNBUFFERED", "1", 1);

        // avoid flood calls to stat(/etc/localtime)
        // http://stackoverflow.com/questions/4554271/how-to-avoid-excessive-stat-etc-localtime-calls-in-strftime-on-linux
        setenv("TZ", ":/etc/localtime", 0);

        // work while we are cd into config_dir
        // to allow the plugins refer to their config
        // files using relative filenames
        if(chdir(netdata_configured_config_dir) == -1)
            fatal("Cannot cd to '%s'", netdata_configured_config_dir);

        char path[1024 + 1], *p = getenv("PATH");
        if(!p) p = "/bin:/usr/bin";
        snprintfz(path, 1024, "%s:%s", p, "/sbin:/usr/sbin:/usr/local/bin:/usr/local/sbin");
        setenv("PATH", config_get("plugins", "PATH environment variable", path), 1);

        p = getenv("PYTHONPATH");
        if(!p) p = "";
        setenv("PYTHONPATH", config_get("plugins", "PYTHONPATH environment variable", p), 1);
    }

    char *user = NULL;
    {
        char *flags = config_get("global", "debug flags",  "0x00000000");
        setenv("NETDATA_DEBUG_FLAGS", flags, 1);

        debug_flags = strtoull(flags, NULL, 0);
        debug(D_OPTIONS, "Debug flags set to '0x%8llx'.", debug_flags);

        if(debug_flags != 0) {
            struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
            if(setrlimit(RLIMIT_CORE, &rl) != 0)
                error("Cannot request unlimited core dumps for debugging... Proceeding anyway...");

#ifdef HAVE_SYS_PRCTL_H
            prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
#endif
        }

        // --------------------------------------------------------------------

#ifdef MADV_MERGEABLE
        enable_ksm = config_get_boolean("global", "memory deduplication (ksm)", enable_ksm);
#else
#warning "Kernel memory deduplication (KSM) is not available"
#endif

        // --------------------------------------------------------------------

        get_system_HZ();
        get_system_cpus();
        get_system_pid_max();
        
        // --------------------------------------------------------------------

        {
            char filename[FILENAME_MAX + 1];
            snprintfz(filename, FILENAME_MAX, "%s/debug.log", netdata_configured_log_dir);
            stdout_filename    = config_get("global", "debug log",  filename);

            snprintfz(filename, FILENAME_MAX, "%s/error.log", netdata_configured_log_dir);
            stderr_filename    = config_get("global", "error log",  filename);

            snprintfz(filename, FILENAME_MAX, "%s/access.log", netdata_configured_log_dir);
            stdaccess_filename = config_get("global", "access log", filename);
        }

        error_log_throttle_period_backup =
            error_log_throttle_period = config_get_number("global", "errors flood protection period", error_log_throttle_period);
        setenv("NETDATA_ERRORS_THROTTLE_PERIOD", config_get("global", "errors flood protection period"    , ""), 1);

        error_log_errors_per_period = (unsigned long)config_get_number("global", "errors to trigger flood protection", error_log_errors_per_period);
        setenv("NETDATA_ERRORS_PER_PERIOD"     , config_get("global", "errors to trigger flood protection", ""), 1);

        if(check_config) {
            stdout_filename = stderr_filename = stdaccess_filename = "system";
            error_log_throttle_period = 0;
            error_log_errors_per_period = 0;
        }
        error_log_limit_unlimited();

        // --------------------------------------------------------------------

        rrd_memory_mode = rrd_memory_mode_id(config_get("global", "memory mode", rrd_memory_mode_name(rrd_memory_mode)));

        // --------------------------------------------------------------------

        {
            char hostnamebuf[HOSTNAME_MAX + 1];
            if(gethostname(hostnamebuf, HOSTNAME_MAX) == -1)
                error("WARNING: Cannot get machine hostname.");
            hostname = config_get("global", "hostname", hostnamebuf);
            debug(D_OPTIONS, "hostname set to '%s'", hostname);
            setenv("NETDATA_HOSTNAME", hostname, 1);
        }

        // --------------------------------------------------------------------

        rrd_default_history_entries = (int) config_get_number("global", "history", RRD_DEFAULT_HISTORY_ENTRIES);
        if(rrd_default_history_entries < 5 || rrd_default_history_entries > RRD_HISTORY_ENTRIES_MAX) {
            error("Invalid history entries %d given. Defaulting to %d.", rrd_default_history_entries, RRD_DEFAULT_HISTORY_ENTRIES);
            rrd_default_history_entries = RRD_DEFAULT_HISTORY_ENTRIES;
        }
        else {
            debug(D_OPTIONS, "save lines set to %d.", rrd_default_history_entries);
        }

        // --------------------------------------------------------------------

        rrd_update_every = (int) config_get_number("global", "update every", UPDATE_EVERY);
        if(rrd_update_every < 1 || rrd_update_every > 600) {
            error("Invalid data collection frequency (update every) %d given. Defaulting to %d.", rrd_update_every, UPDATE_EVERY_MAX);
            rrd_update_every = UPDATE_EVERY;
        }
        else debug(D_OPTIONS, "update timer set to %d.", rrd_update_every);

        // let the plugins know the min update_every
        {
            char buf[16];
            snprintfz(buf, 15, "%d", rrd_update_every);
            setenv("NETDATA_UPDATE_EVERY", buf, 1);
        }

        // --------------------------------------------------------------------

        // block signals while initializing threads.
        // this causes the threads to block signals.
        sigset_t sigset;
        sigfillset(&sigset);
        if(pthread_sigmask(SIG_BLOCK, &sigset, NULL) == -1)
            error("Could not block signals for threads");

        // Catch signals which we want to use
        struct sigaction sa;
        sa.sa_flags = 0;

        // ingore all signals while we run in a signal handler
        sigfillset(&sa.sa_mask);

        // INFO: If we add signals here we have to unblock them
        // at popen.c when running a external plugin.

        // Ignore SIGPIPE completely.
        sa.sa_handler = SIG_IGN;
        if(sigaction(SIGPIPE, &sa, NULL) == -1)
            error("Failed to change signal handler for SIGPIPE");

        sa.sa_handler = sig_handler_exit;
        if(sigaction(SIGINT, &sa, NULL) == -1)
            error("Failed to change signal handler for SIGINT");

        sa.sa_handler = sig_handler_exit;
        if(sigaction(SIGTERM, &sa, NULL) == -1)
            error("Failed to change signal handler for SIGTERM");

        sa.sa_handler = sig_handler_logrotate;
        if(sigaction(SIGHUP, &sa, NULL) == -1)
            error("Failed to change signal handler for SIGHUP");

        // save database on SIGUSR1
        sa.sa_handler = sig_handler_save;
        if(sigaction(SIGUSR1, &sa, NULL) == -1)
            error("Failed to change signal handler for SIGUSR1");

        // reload health configuration on SIGUSR2
        sa.sa_handler = sig_handler_reload_health;
        if(sigaction(SIGUSR2, &sa, NULL) == -1)
            error("Failed to change signal handler for SIGUSR2");

        // --------------------------------------------------------------------

        i = pthread_attr_init(&attr);
        if(i != 0)
            fatal("pthread_attr_init() failed with code %d.", i);

        i = pthread_attr_getstacksize(&attr, &stacksize);
        if(i != 0)
            fatal("pthread_attr_getstacksize() failed with code %d.", i);
        else
            debug(D_OPTIONS, "initial pthread stack size is %zu bytes", stacksize);

        wanted_stacksize = (size_t)config_get_number("global", "pthread stack size", (long)stacksize);

        // --------------------------------------------------------------------

        for (i = 0; static_threads[i].name != NULL ; i++) {
            struct netdata_static_thread *st = &static_threads[i];

            if(st->config_name) st->enabled = config_get_boolean(st->config_section, st->config_name, st->enabled);
            if(st->enabled && st->init_routine) st->init_routine();
        }

        // --------------------------------------------------------------------

        // get the user we should run
        // IMPORTANT: this is required before web_files_uid()
        user = config_get("global", "run as user"    , (getuid() == 0)?NETDATA_USER:"");

        // IMPORTANT: these have to run once, while single threaded
        web_files_uid(); // IMPORTANT: web_files_uid() before web_files_gid()
        web_files_gid();

        // --------------------------------------------------------------------

        if(!check_config)
            create_listen_sockets();
    }

    // initialize the log files
    open_all_log_files();

#ifdef NETDATA_INTERNAL_CHECKS
    if(debug_flags != 0) {
        struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
        if(setrlimit(RLIMIT_CORE, &rl) != 0)
            error("Cannot request unlimited core dumps for debugging... Proceeding anyway...");
#ifdef HAVE_SYS_PRCTL_H
        prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
#endif
    }
#endif /* NETDATA_INTERNAL_CHECKS */

    // fork, switch user, create pid file, set process priority
    if(become_daemon(dont_fork, user) == -1)
        fatal("Cannot daemonize myself.");

    info("netdata started on pid %d.", getpid());

    // ------------------------------------------------------------------------
    // get default pthread stack size

    if(stacksize < wanted_stacksize) {
        i = pthread_attr_setstacksize(&attr, wanted_stacksize);
        if(i != 0)
            fatal("pthread_attr_setstacksize() to %zu bytes, failed with code %d.", wanted_stacksize, i);
        else
            debug(D_SYSTEM, "Successfully set pthread stacksize to %zu bytes", wanted_stacksize);
    }

    // ------------------------------------------------------------------------
    // initialize rrd host

    rrdhost_init(hostname);

    // ------------------------------------------------------------------------
    // initialize the registry

    registry_init();

    // ------------------------------------------------------------------------
    // initialize health monitoring

    health_init();

    if(check_config)
        exit(1);

    // ------------------------------------------------------------------------
    // enable log flood protection

    error_log_limit_reset();

    // ------------------------------------------------------------------------
    // spawn the threads

    web_server_threading_selection();

    for (i = 0; static_threads[i].name != NULL ; i++) {
        struct netdata_static_thread *st = &static_threads[i];

        if(st->enabled) {
            st->thread = mallocz(sizeof(pthread_t));

            debug(D_SYSTEM, "Starting thread %s.", st->name);

            if(pthread_create(st->thread, &attr, st->start_routine, st))
                error("failed to create new thread for %s.", st->name);

            else if(pthread_detach(*st->thread))
                error("Cannot request detach of newly created %s thread.", st->name);
        }
        else debug(D_SYSTEM, "Not starting thread %s.", st->name);
    }

    info("netdata initialization completed. Enjoy real-time performance monitoring!");

    // ------------------------------------------------------------------------
    // block signals while initializing threads.
    sigset_t sigset;
    sigfillset(&sigset);

    if(pthread_sigmask(SIG_UNBLOCK, &sigset, NULL) == -1) {
        error("Could not unblock signals for threads");
    }

    // Handle flags set in the signal handler.
    while(1) {
        pause();
        if(netdata_exit) {
            debug(D_EXIT, "Exit main loop of netdata.");
            netdata_cleanup_and_exit(0);
            exit(0);
        }
    }
}
