#ifndef OPENCE_TV_SWARM_TEST_H
#define OPENCE_TV_SWARM_TEST_H
#ifdef __cplusplus
extern "C" {
#endif
/* Asset-free loopback test. No DHT, trackers, router mappings or public peers.
   Returns 0 only after both hash verification and byte comparison succeed. */
int opence_probe_tcp(const char *temporary_directory, char *report, unsigned int capacity);
#ifdef __cplusplus
}
#endif
#endif
