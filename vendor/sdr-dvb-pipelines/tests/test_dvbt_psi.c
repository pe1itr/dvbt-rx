/* A player joining/reopening live DVB-T output must still receive PAT/PMT.
 * Exercise the actual output gate after the validator identified a service. */
#include "../components/dvbt/src/dvbt_outer.c"
#include <assert.h>

static void packet(uint8_t *out, unsigned pid, unsigned cc, int start) {
    memset(out, 0xff, RS_DATA_LEN);
    out[0] = 0x47;
    out[1] = (pid >> 8) | (start ? 0x40 : 0);
    out[2] = pid;
    out[3] = 0x10 | cc;
    out[4] = 0; /* PSI pointer field for section starts. */
}

static void check(int open_on_psi) {
    ts_output_gate_t gate;
    ts_output_gate_init(&gate, 0);
    ts_validator_t validator = {.pat_packets=1, .pmt_packets=1, .pmt_pid=4096,
        .video_pid=256, .video_stream_type=0x1b};
    ts_output_sink_t sink = {.file=tmpfile()};
    assert(sink.file);
    uint8_t input[7][RS_DATA_LEN], output[sizeof(input)];
    packet(input[0], 0, 0, 1);
    packet(input[1], 4096, 0, 1);
    packet(input[2], 256, 0, 0);
    packet(input[3], 0, 1, 1);    /* Next PAT version/section. */
    packet(input[4], 0, 2, 0);    /* Continuation must survive too. */
    packet(input[5], 4096, 1, 1); /* Next PMT. */
    packet(input[6], 4096, 2, 0);
    uint32_t written = 0;
    for (unsigned i = 0; i < 7; ++i) {
        int locked = open_on_psi || i >= 2;
        assert(!ts_output_gate_try_write(&gate, &validator, &sink, "test",
                                        input[i], locked, &written));
    }
    assert(!ts_output_gate_flush(&gate, &sink, "test"));
    assert(written == 7); /* Bootstrap exactly once; later PSI stays intact. */
    rewind(sink.file);
    assert(fread(output, 1, sizeof(output), sink.file) == sizeof(output));
    assert(!memcmp(input, output, sizeof(input)));
    assert(fgetc(sink.file) == EOF);
    fclose(sink.file);
}

static void check_private_ts(void) {
    for (int raw=0;raw<=1;++raw) {
        ts_output_gate_t gate;ts_output_gate_init(&gate,1);
        ts_validator_t validator;ts_validator_init(&validator);
        ts_output_sink_t sink={.file=tmpfile()};assert(sink.file);
        rbdvbt_status_context_t status={.raw_ts_output=raw,.wait_video_start=1};
        rbdvbt_ts_packet_t item={0};uint32_t written=0;rx_lock_state_t lock=RX_LOCK_SEARCH;
        packet(item.bytes,8190,0,0);uint8_t expected[188];memcpy(expected,item.bytes,188);
        assert(!block4_consume_fifo3_packet(&item,&gate,&validator,&sink,"test",1,1,&lock,&status,&written));
        assert(written==(unsigned)raw);
        /* TEI and sync failures must still be discarded in raw mode. */
        item.bytes[1]|=0x80;
        assert(!block4_consume_fifo3_packet(&item,&gate,&validator,&sink,"test",1,1,&lock,&status,&written));
        item.bytes[0]=0;
        assert(!block4_consume_fifo3_packet(&item,&gate,&validator,&sink,"test",1,1,&lock,&status,&written));
        assert(written==(unsigned)raw);
        assert(!ts_output_gate_flush(&gate,&sink,"test"));rewind(sink.file);
        if (raw) {uint8_t actual[188];assert(fread(actual,1,188,sink.file)==188);assert(!memcmp(expected,actual,188));}
        assert(fgetc(sink.file)==EOF);fclose(sink.file);
    }
}
int main(void) {
    check_private_ts();
    check(0);
    check(1);
    puts("DVB-T live PSI: initial tables once, repeated/continued PAT and PMT preserved");
    return 0;
}
