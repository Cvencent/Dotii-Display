#include <stdbool.h>

#include "connectivity.h"

/* The protocol test app does not run the Wi-Fi/HTTP service, but the CBDP
   client links against it. Keep the two entry points it needs as inert stubs so
   the protocol tests exercise networking state transitions without a radio. */
bool connectivity_is_wifi_connected(void)
{
    return true;
}

void connectivity_request_refresh(void)
{
}

/* The CBDP client reports the address a datagram arrived from. The protocol
   test app has no HTTP service here, so this candidate is never promoted. */
void connectivity_note_bridge_ipv4(const char *ipv4)
{
    (void)ipv4;
}
