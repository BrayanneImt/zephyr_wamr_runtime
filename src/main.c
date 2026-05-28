#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "wasm_export.h"

/* ----------------------------------------------------------------
 * Nœud UART — console série utilisée pour recevoir le .wasm
 * ---------------------------------------------------------------- */
#define UART_NODE DT_CHOSEN(zephyr_console)

/* ----------------------------------------------------------------
 * Tailles mémoire
 *
 * WASM_MAX_SIZE : taille maximale acceptée pour le fichier .wasm
 *                (128 Ko couvre les .wasm C et Rust de ce projet)
 *
 * STACK_SIZE    : stack alloué à l'instance WASM et à l'exec env
 *                 32 Ko évite les stack overflow sur des .wasm
 *                 comportant de la récursion ou de grandes frames.
 *
 * HEAP_SIZE     : heap linéaire de l'instance WASM
 *                 64 Ko suffisent pour les buffers socket (512 o)
 *                 et les chaînes de l'application HTTP.
 * ---------------------------------------------------------------- */
#define WASM_MAX_SIZE (128 * 1024)
#define STACK_SIZE    (16  * 1024)
#define HEAP_SIZE     (32  * 1024)

/* ----------------------------------------------------------------
 * Buffers statiques
 *
 * wasm_buffer  : reçoit le binaire .wasm envoyé par UART
 * global_heap  : pool mémoire donné à WAMR à l'initialisation
 *                (WAMR y alloue ses structures internes)
 * ---------------------------------------------------------------- */
static uint8_t wasm_buffer[WASM_MAX_SIZE];
static char    global_heap[HEAP_SIZE];

/* Handle UART */
static const struct device *uart_dev;

/* ----------------------------------------------------------------
 * uart_read_byte() — lecture bloquante d'un octet sur l'UART
 *
 * uart_poll_in() retourne -1 si aucun octet n'est disponible.
 * On boucle jusqu'à recevoir effectivement un octet, en cédant
 * le CPU à chaque itération via k_yield() pour ne pas bloquer
 * les autres threads Zephyr.
 * ---------------------------------------------------------------- */
static void uart_read_byte(const struct device *dev, uint8_t *out)
{
    while (uart_poll_in(dev, out) != 0) {
        k_yield();
    }
}

/* ----------------------------------------------------------------
 * execute_wasm() — charge et exécute un module WASM
 *
 * Séquence WAMR :
 *   wasm_runtime_load()           → parse et valide le binaire
 *   wasm_runtime_instantiate()    → alloue mémoire linéaire + stack
 *   wasm_runtime_create_exec_env()→ crée l'environnement d'exécution
 *   wasm_runtime_lookup_function()→ cherche la fonction "_start"
 *   wasm_runtime_call_wasm()      → exécute la fonction
 * ---------------------------------------------------------------- */
static void execute_wasm(uint8_t *wasm_data, uint32_t wasm_size)
{
    char error_buf[128];

    wasm_module_t          module      = NULL;
    wasm_module_inst_t     module_inst = NULL;
    wasm_exec_env_t        exec_env    = NULL;
    wasm_function_inst_t   func        = NULL;

    /* 1. Charger le module WASM */
    module = wasm_runtime_load(
        wasm_data, wasm_size,
        error_buf, sizeof(error_buf));

    if (!module) {
        printk("LOAD ERROR: %s\n", error_buf);
        return;
    }
    printk("Module charge OK\n");

    /* 2. Instancier (alloue la mémoire linéaire WASM) */
    module_inst = wasm_runtime_instantiate(
        module,
        STACK_SIZE,
        HEAP_SIZE,
        error_buf, sizeof(error_buf));

    if (!module_inst) {
        printk("INSTANTIATE ERROR: %s\n", error_buf);
        goto cleanup_module;
    }
    printk("Instance creee OK\n");

    /* 3. Créer l'environnement d'exécution */
    exec_env = wasm_runtime_create_exec_env(module_inst, STACK_SIZE);

    if (!exec_env) {
        printk("EXEC ENV FAILED\n");
        goto cleanup_inst;
    }

    /* 4. Rechercher la fonction _start
     *
     * CORRECTION : wasm_runtime_lookup_function() n'accepte plus
     * que 2 arguments depuis WAMR >= 1.3.
     * Ancien prototype : lookup_function(inst, name, signature)
     * Nouveau prototype : lookup_function(inst, name)
     */
    func = wasm_runtime_lookup_function(module_inst, "_start");

    if (!func) {
        printk("_start not found\n");
        goto cleanup_env;
    }

    /* 5. Exécuter */
    printk("Executing WASM...\n");

    if (!wasm_runtime_call_wasm(exec_env, func, 0, NULL)) {
        printk("EXCEPTION: %s\n",
               wasm_runtime_get_exception(module_inst));
    } else {
        printk("Execution completed\n");
    }

cleanup_env:
    wasm_runtime_destroy_exec_env(exec_env);

cleanup_inst:
    wasm_runtime_deinstantiate(module_inst);

cleanup_module:
    wasm_runtime_unload(module);
}

/* ----------------------------------------------------------------
 * main()
 * ---------------------------------------------------------------- */
int main(void)
{
    /* 1. Initialiser WAMR avec pool mémoire statique */
    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));

    init_args.mem_alloc_type                    = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf    = global_heap;
    init_args.mem_alloc_option.pool.heap_size   = sizeof(global_heap);

    if (!wasm_runtime_full_init(&init_args)) {
        printk("WAMR init failed\n");
        return -1;
    }
    printk("WAMR init OK\n");

    /* 2. Récupérer le périphérique UART */
    uart_dev = DEVICE_DT_GET(UART_NODE);

    if (!device_is_ready(uart_dev)) {
        printk("UART not ready\n");
        return -1;
    }

    printk("\n");
    printk("===== WAMR UART DEPLOYMENT =====\n");
    printk("Protocol : 4 bytes size (LE) + wasm binary\n");
    printk("Max size : %d bytes\n", WASM_MAX_SIZE);

    /* 3. Boucle principale : attente upload → exécution */
    while (1) {
        uint32_t wasm_size = 0;

        printk("\nWaiting upload...\n");

        /* Lire les 4 octets de taille (little-endian)
         *
         * CORRECTION : utiliser uart_read_byte() qui attend
         * activement un octet disponible, au lieu de uart_poll_in()
         * brut qui retourne -1 si rien n'est dispo et lirait 0xFF.
         */
        for (int i = 0; i < 4; i++) {
            uint8_t b;
            uart_read_byte(uart_dev, &b);
            ((uint8_t *)&wasm_size)[i] = b;
        }

        printk("Incoming size = %u bytes\n", wasm_size);

        /* Vérifier que la taille est dans les limites */
        if (wasm_size == 0 || wasm_size > WASM_MAX_SIZE) {
            printk("ERROR: invalid size (0 < size <= %d)\n",
                   WASM_MAX_SIZE);
            continue;
        }

        /* Lire le binaire .wasm octet par octet */
        for (uint32_t i = 0; i < wasm_size; i++) {
            uart_read_byte(uart_dev, &wasm_buffer[i]);
        }

        printk("Upload complete (%u bytes)\n", wasm_size);

        /* Charger et exécuter */
        execute_wasm(wasm_buffer, wasm_size);
    }

    return 0;
}