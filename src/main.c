#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <errno.h>

#include "wasm_export.h"

#define UART_NODE DT_CHOSEN(zephyr_console)
#define LED_NODE  DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED_NODE, okay)
static const struct gpio_dt_spec board_led = GPIO_DT_SPEC_GET(LED_NODE, gpios);
#define HAS_LED 1
#else
#define HAS_LED 0
#endif

#define WASM_MAX_SIZE  (32 * 1024)
#define WAMR_POOL_SIZE (110 * 1024)
#define STACK_SIZE     (8  * 1024)
#define HEAP_SIZE      (16 * 1024)

static uint8_t wasm_buffer[WASM_MAX_SIZE];
static char    global_heap[WAMR_POOL_SIZE];
static const struct device *uart_dev;

static K_SEM_DEFINE(net_ready_wamr, 0, 1);
static struct net_mgmt_event_callback dhcp_cb_wamr;

static void on_dhcp_wamr(struct net_mgmt_event_callback *cb,
                          uint64_t event, struct net_if *iface)
{
    if (event == NET_EVENT_IPV4_DHCP_BOUND) {
        k_sem_give(&net_ready_wamr);
    }
}

static void uart_read_byte(const struct device *dev, uint8_t *out)
{
    while (uart_poll_in(dev, out) != 0) { k_yield(); }
}

static void uart_drain_rx(const struct device *dev)
{
    uint8_t dummy;
    int drained;
    do {
        drained = 0;
        while (uart_poll_in(dev, &dummy) == 0) { drained++; }
        if (drained > 0) { k_msleep(200); }
    } while (drained > 0);
}

/* ==============================================================
 * HOST FUNCTIONS
 * ============================================================== */

static int32_t host_wifi_connect_impl(wasm_exec_env_t exec_env,
                                       uint32_t ssid_ptr, uint32_t ssid_len,
                                       uint32_t psk_ptr,  uint32_t psk_len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const char *ssid = (const char *)wasm_runtime_addr_app_to_native(inst, ssid_ptr);
    const char *psk  = (const char *)wasm_runtime_addr_app_to_native(inst, psk_ptr);
    if (!ssid || !psk) { return -1; }

    struct net_if *iface = net_if_get_default();
    if (!iface) { return -1; }

    net_mgmt_init_event_callback(&dhcp_cb_wamr, on_dhcp_wamr, NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&dhcp_cb_wamr);

    struct wifi_connect_req_params params = {
        .ssid        = (const uint8_t *)ssid,
        .ssid_length = (uint8_t)ssid_len,
        .psk         = (const uint8_t *)psk,
        .psk_length  = (uint8_t)psk_len,
        .channel     = WIFI_CHANNEL_ANY,
        .security    = WIFI_SECURITY_TYPE_PSK,
        .mfp         = WIFI_MFP_OPTIONAL,
        .band        = WIFI_FREQ_BAND_2_4_GHZ,
        .timeout     = SYS_FOREVER_MS,
    };

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    if (ret == 0) { net_dhcpv4_start(iface); }
    return ret;
}

static int32_t host_wait_network_ready_impl(wasm_exec_env_t exec_env,
                                             uint32_t timeout_secs)
{
    ARG_UNUSED(exec_env);
    return (k_sem_take(&net_ready_wamr, K_SECONDS(timeout_secs)) == 0) ? 0 : -1;
}

static void host_gpio_blink_impl(wasm_exec_env_t exec_env)
{
    ARG_UNUSED(exec_env);
#if HAS_LED
    if (device_is_ready(board_led.port)) {
        gpio_pin_set_dt(&board_led, 1); k_msleep(150);
        gpio_pin_set_dt(&board_led, 0); k_msleep(150);
    }
#endif
}

static int32_t host_tcp_connect_impl(wasm_exec_env_t exec_env,
                                      uint32_t ip_ptr, uint32_t ip_len,
                                      uint32_t port,   uint32_t timeout_secs)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const char *ip = (const char *)wasm_runtime_addr_app_to_native(inst, ip_ptr);
    if (!ip) { return -1; }

    /* Copier l'IP dans un buffer null-terminé */
    char ip_str[32];
    uint32_t copy_len = ip_len < sizeof(ip_str) - 1 ? ip_len : sizeof(ip_str) - 1;
    memcpy(ip_str, ip, copy_len);
    ip_str[copy_len] = '\0';

    int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { return -1; }

    struct zsock_timeval tv = { .tv_sec = timeout_secs, .tv_usec = 0 };
    zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (zsock_inet_pton(AF_INET, ip_str, &addr.sin_addr) != 1) {
        zsock_close(sock);
        return -1;
    }

    if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        zsock_close(sock);
        return -1;
    }

    return sock;
}

static int32_t host_tcp_send_impl(wasm_exec_env_t exec_env,
                                   int32_t fd, uint32_t buf_ptr, uint32_t buf_len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    const uint8_t *buf = (const uint8_t *)wasm_runtime_addr_app_to_native(inst, buf_ptr);
    if (!buf) { return -1; }
    return zsock_send(fd, buf, buf_len, 0);
}

static int32_t host_tcp_recv_impl(wasm_exec_env_t exec_env,
                                   int32_t fd, uint32_t buf_ptr, uint32_t buf_len)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    uint8_t *buf = (uint8_t *)wasm_runtime_addr_app_to_native(inst, buf_ptr);
    if (!buf) { return -1; }
    return zsock_recv(fd, buf, buf_len, 0);
}

static void host_tcp_close_impl(wasm_exec_env_t exec_env, int32_t fd)
{
    ARG_UNUSED(exec_env);
    zsock_close(fd);
}

static void host_sleep_impl(wasm_exec_env_t exec_env, uint32_t secs)
{
    ARG_UNUSED(exec_env);
    k_sleep(K_SECONDS(secs));
}

static NativeSymbol native_symbols[] = {
    { "host_wifi_connect",       host_wifi_connect_impl,       "(iiii)i", NULL },
    { "host_wait_network_ready", host_wait_network_ready_impl, "(i)i",    NULL },
    { "host_gpio_blink",         host_gpio_blink_impl,         "()",      NULL },
    { "host_tcp_connect",        host_tcp_connect_impl,        "(iiii)i", NULL },
    { "host_tcp_send",           host_tcp_send_impl,           "(iii)i",  NULL },
    { "host_tcp_recv",           host_tcp_recv_impl,           "(iii)i",  NULL },
    { "host_tcp_close",          host_tcp_close_impl,          "(i)",     NULL },
    { "host_sleep",              host_sleep_impl,              "(i)",     NULL },
};

/* ----------------------------------------------------------------
 * execute_wasm()
 * ---------------------------------------------------------------- */
static void execute_wasm(uint8_t *wasm_data, uint32_t wasm_size)
{
    char error_buf[128];
    wasm_module_t        module      = NULL;
    wasm_module_inst_t   module_inst = NULL;
    wasm_exec_env_t      exec_env    = NULL;
    wasm_function_inst_t func        = NULL;

    module = wasm_runtime_load(wasm_data, wasm_size, error_buf, sizeof(error_buf));
    if (!module) { printk("LOAD ERROR: %s\n", error_buf); return; }
    printk("Module charge OK\n");

    module_inst = wasm_runtime_instantiate(module, STACK_SIZE, HEAP_SIZE,
                                           error_buf, sizeof(error_buf));
    if (!module_inst) { printk("INSTANTIATE ERROR: %s\n", error_buf); goto cleanup_module; }
    printk("Instance creee OK\n");

    exec_env = wasm_runtime_create_exec_env(module_inst, STACK_SIZE);
    if (!exec_env) { printk("EXEC ENV FAILED\n"); goto cleanup_inst; }

    func = wasm_runtime_lookup_function(module_inst, "_start");
    if (!func) { printk("_start not found\n"); goto cleanup_env; }

    printk("Executing WASM...\n");
    if (!wasm_runtime_call_wasm(exec_env, func, 0, NULL)) {
        printk("EXCEPTION: %s\n", wasm_runtime_get_exception(module_inst));
    } else {
        printk("Execution completed\n");
    }

cleanup_env:    wasm_runtime_destroy_exec_env(exec_env);
cleanup_inst:   wasm_runtime_deinstantiate(module_inst);
cleanup_module: wasm_runtime_unload(module);
}

/* ----------------------------------------------------------------
 * main()
 * ---------------------------------------------------------------- */
int main(void)
{
#if HAS_LED
    if (device_is_ready(board_led.port)) {
        gpio_pin_configure_dt(&board_led, GPIO_OUTPUT_INACTIVE);
    }
#endif

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type                  = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf  = global_heap;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap);

    if (!wasm_runtime_full_init(&init_args)) { printk("WAMR init failed\n"); return -1; }
    printk("WAMR init OK\n");

    if (!wasm_runtime_register_natives("env", native_symbols, ARRAY_SIZE(native_symbols))) {
        printk("Failed to register native symbols\n"); return -1;
    }
    printk("Host functions enregistrees OK\n");

    uart_dev = DEVICE_DT_GET(UART_NODE);
    if (!device_is_ready(uart_dev)) { printk("UART not ready\n"); return -1; }

    printk("\n===== WAMR UART DEPLOYMENT =====\n");
    printk("Protocol : 4 bytes size (LE) + wasm binary\n");
    printk("Max size : %d bytes\n", WASM_MAX_SIZE);

    while (1) {
        uint32_t wasm_size = 0;
        printk("\nWaiting upload...\n");

        for (int i = 0; i < 4; i++) {
            uint8_t b;
            uart_read_byte(uart_dev, &b);
            ((uint8_t *)&wasm_size)[i] = b;
        }

        printk("Incoming size = %u bytes\n", wasm_size);

        if (wasm_size == 0 || wasm_size > WASM_MAX_SIZE) {
            printk("ERROR: invalid size (0 < size <= %d)\n", WASM_MAX_SIZE);
            uart_drain_rx(uart_dev);
            continue;
        }

        for (uint32_t i = 0; i < wasm_size; i++) {
            uart_read_byte(uart_dev, &wasm_buffer[i]);
        }

        printk("Upload complete (%u bytes)\n", wasm_size);
        execute_wasm(wasm_buffer, wasm_size);
    }

    return 0;
}