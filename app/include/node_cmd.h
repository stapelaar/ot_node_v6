#pragma once

/* Polt NDxx/IN/CMD via CoAP GET en voert het commando uit indien aanwezig.
 * Aanroepen vanuit de sample loop na elke publish cyclus. */
void node_cmd_poll(const char *root);
