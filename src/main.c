#include "config.h"
#include "receiver.h"

#include <stdio.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>

static void rbdvbt_set_binary_stdio(void)
{
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
}
#else
static void rbdvbt_set_binary_stdio(void)
{
}
#endif

int main(int argc, char **argv)
{
    rbdvbt_config_t cfg;

    rbdvbt_set_binary_stdio();

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
