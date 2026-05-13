#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/wifi_mgmt.h>

// event callbacks
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

// semaphores
static K_SEM_DEFINE(sem_wifi, 0, 1);
static K_SEM_DEFINE(sem_ipv4, 0, 1);

// called when wifi is connected  by signature defined in zephyr
/**
 * @typedef net_mgmt_event_handler_t
 * @brief Define the user's callback handler function signature
 * @param cb Original struct net_mgmt_event_callback owning this handler.
 * @param mgmt_event The network event being notified.
 * @param iface A pointer on a struct net_if to which the event belongs to,
 *        if it's an event on an ixface. NULL otherwise.
 */
static void on_wifi_connection_event (struct net_mgmt_event_callback *cb,
					                  uint64_t mgmt_event,
					                  struct net_if *iface) {
    const struct wifi_status *status = (const struct wifi_status *)cb -> info; // pull the info from callback

    if(mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
        if (status->status) {
            printk("Error (%d): Connection request failed \r\n", status -> status);
        } else {
            printk("Connected \r\n");
            k_sem_give(&sem_wifi);
        }
    } else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        if (status->status) {
            printk("Error (%d): Disconnection request faild \r\n", status ->status);
        } else {
            printk("Disconnected \r\n");
            k_sem_take(&sem_wifi, K_NO_WAIT);
        }
    }
}

static void on_ipv4_obtained (struct net_mgmt_event_callback *cb,
                              uint64_t mgmt_event,
                              struct net_if *iface) {
    // signal that the ip address has been obtained
    if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
        k_sem_give(&sem_ipv4);
    }
}

// initialize the wifi event callbacks

void wifi_init(void){
    //initalize the event cb, in func without () there is no need to put & in front
    net_mgmt_init_event_callback(&wifi_cb, on_wifi_connection_event, NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);

    net_mgmt_init_event_callback(&ipv4_cb, on_ipv4_obtained, NET_EVENT_IPV4_ADDR_ADD);

    // add event cb
    net_mgmt_add_event_callback(&wifi_cb);
    net_mgmt_add_event_callback(&ipv4_cb);
}

// connect to wifi network (blocking)

int wifi_connect(char *ssid, char *psk) {

    int ret;
    struct net_if *iface;
    struct wifi_connect_req_params params;

    // get the default networking interface
    iface = net_if_get_default();               // in esp32 this should be enough - wifi interface

    // fill in the connection rquest param and cast them by pointers into uint cause of web connecting standards
    params.ssid = (const uint8_t *)ssid;        //name of wifi
    params.ssid_length = strlen(ssid);
    params.psk = (const uint8_t *)psk;          // password of wifi
    params.psk_length = strlen(psk);
    params.security = WIFI_SECURITY_TYPE_PSK;   // type of  encription used by router
    params.band = WIFI_FREQ_BAND_UNKNOWN;       // band freq 2.4 or 5.0
    params.channel = WIFI_CHANNEL_ANY;          // search for right channel
    params.mfp = WIFI_MFP_OPTIONAL;             // managment frame protection - optional y/n

    // connect to  the wifi network
    ret = net_mgmt(NET_REQUEST_WIFI_CONNECT,
                   iface,
                   &params,
                   sizeof(params));
    // wait for the connection to complete
    k_sem_take(&sem_wifi, K_FOREVER);

    return ret;
}

// wait for ip address (blocking)
void wifi_wait_for_ip_addr(void) {
    struct wifi_iface_status status;
    struct net_if *iface;
    char ip_addr[NET_IPV4_ADDR_LEN];
    char gw_addr[NET_IPV4_ADDR_LEN];

    //get iface
    iface = net_if_get_default();

    //wait for  the ipv4 address to be obtained
    k_sem_take(&sem_ipv4, K_FOREVER);

    // get wifi status
    if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS,
                 iface,
                 &status,
                 sizeof(struct wifi_iface_status))) {
        printk("Error: wifi status request failed \r\n");
    }

    //get the ip address
    memset(ip_addr, 0, sizeof(ip_addr));    //clearing ip addr to avoid garbage char 
    if (net_addr_ntop(AF_INET,             //network address network to printable binary to readable
                      &iface->config.ip.ipv4->unicast[0].ipv4.address.in_addr,  //unicast - first ipv4 address
                      ip_addr,
                      sizeof(ip_addr)) == NULL){
        printk("Error: could not convert ip address to string \r\n");
    }
    //get the gw address to enable routing outide local network
    memset(gw_addr, 0, sizeof(gw_addr));
    if (net_addr_ntop(AF_INET,
                      &iface->config.ip.ipv4->gw,
                      gw_addr,
                      sizeof(gw_addr)) == NULL) {
        printk("Error: could not convert gateway address to string \r\n");
    }

    // print wifi status
    printk("Wifi status: \r\n");
    if (status.state >= WIFI_STATE_ASSOCIATED) {
        printk("SSID: %-32s\r\n", status.ssid);
        printk("Band: %d\r\n", status.band);
        printk("Channel: %d\r\n", status.channel);
        printk("Security: %d\r\n", status.security);
        printk("Ip address: %s\r\n", ip_addr);
        printk("Gateway: %s\r\n", gw_addr);
    }
}

//disconnect from wifi

int wifi_disconnect(void) {
    int ret;
    struct net_if *iface = net_if_get_default();

    ret = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);

    return ret;
}
