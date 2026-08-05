#pragma once
// Wildcard captive-portal DNS responder for AP-onboarding mode: answers every
// A/ANY query with the SoftAP IP (192.168.4.1) so OS captive-portal probes hit
// this device. Start when the AP comes up, stop when it goes down. Idempotent.
void captive_dns_start(void);
void captive_dns_stop(void);
