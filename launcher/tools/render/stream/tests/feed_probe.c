/* feed_probe: feeds a recording in caller-sized pieces and reports replay checks and pixels. */
#include <stdio.h>
#include <stdlib.h>

#include "hash/crc32.h"
#include "stream/stream_provider.h"

int
main(int argc, char** argv) {
    if (argc != 4) {
        return 2;
    }
    FILE* file = fopen(argv[1], "rb");
    const size_t chunk = (size_t)strtoul(argv[2], NULL, 10);
    uint8_t* bytes = malloc(chunk);
    if (file == NULL || bytes == NULL || chunk == 0) {
        return 2;
    }
    stream_forget();
    bool ok = true;
    size_t count;
    while (ok && (count = fread(bytes, 1, chunk, file)) > 0) {
        ok = stream_feed(bytes, count);
    }
    fclose(file);
    free(bytes);
    stream_status_t status = stream_status();
    printf("INDEX %d %d %s %s\n", status.frames, status.dropped, status.build_id, status.why);
    if (!ok) {
        stream_forget();
        return 1;
    }
    char* args[] = {argv[0], "--check", "-o", argv[3]};
    int code;
    if (!render_host_open(&stream_provider, sizeof args / sizeof args[0], args, &code)) {
        return code;
    }
    if (status.frames > 0) {
        if (!stream_seek(status.frames - 1) || stream_next() != status.frames - 1 || !stream_seek(0)) {
            return 2;
        }
    }
    while (render_host_frame()->index < render_host_frame()->count) {
        render_host_step(render_host_frame()->dt_ms, &render_host_frame()->input);
    }
    const render_host_image_t image = render_host_image();
    const uint32_t crc = crc32_update(0, image.bgr, image.stride * (size_t)image.height);
    const bool closed = render_host_close();
    status = stream_status();
    printf("RESULT %d %d %08x\n", status.checks_same, status.checks_differ, crc);
    stream_forget();
    return closed ? 0 : 1;
}
