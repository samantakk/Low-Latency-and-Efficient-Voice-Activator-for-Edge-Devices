#pragma once

/*
 * stats_task (diagnostics). Woken by audio_ingest_task every CONFIG_SIH_STATS_PERIOD_HOPS
 * hops, so it adds no timer of its own. Prints per-core idle % (the <10% CPU budget),
 * hop timing from dsp_task, audio level, NN invocations, heap and stack headroom.
 */
void stats_task(void *arg);
