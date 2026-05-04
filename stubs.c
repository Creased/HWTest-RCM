/*
 * Stubs for bdk symbols pulled in by code paths hwtest never executes.
 *
 * hw_init.c calls `minerva_deinit` from `hw_deinit`. We never call
 * hw_deinit, but the linker still pulls in the reference; stub it so the
 * payload links.
 */

void minerva_deinit(void) {}
