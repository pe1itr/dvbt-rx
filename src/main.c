#include "config.h"
#include "receiver.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    rbdvbt_config_t cfg;

    if (rbdvbt_parse_args(argc, argv, &cfg) != 0) {
        return 2;
    }
    if (cfg.show_help) {
        rbdvbt_print_info(argv[0]);
        return 0;
    }
    if (cfg.show_version) {
        printf("rbdvbt_rx %s\n", RBDVBT_VERSION);
        return 0;
    }

    return rbdvbt_run_receiver(&cfg);
}
