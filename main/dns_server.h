#ifndef DNS_SERVER_H
#define DNS_SERVER_H

void dns_server_start(const char *ap_ip);
void dns_server_stop(void);
void dns_server_set_ap_ip(const char *ip);
void dns_server_run(void); // DNS server main loop (called from central task)

#endif // DNS_SERVER_H
