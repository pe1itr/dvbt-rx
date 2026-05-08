#include "config.h"
#include "receiver.h"

int main(int argc, char **argv)
{
    rbdvbt_config_t cfg;

    if (rbdvbt_parse_args(argc, argv, &cfg) != 0) {
        return 2;
    }

    return rbdvbt_run_receiver(&cfg);
}
