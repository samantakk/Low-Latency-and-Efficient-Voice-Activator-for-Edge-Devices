# Pure C component: ring buffer + sample format helpers.
# No ESP-IDF dependencies on purpose, so the same sources are unit tested on a PC
# (see test/host/run_tests.sh).
idf_component_register(SRCS "ring_buffer.c"
                       INCLUDE_DIRS "include")
