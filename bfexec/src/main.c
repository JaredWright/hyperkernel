/**
 * Bareflank Hyperkernel
 * Copyright (C) 2018 Assured Information Security, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <bfaffinity.h>
#include <bfelf_loader.h>

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/mman.h>

#include <hypercall.h>
#include <gpa_layout.h>

#include "xen/start_info.h"

// Notes:
//
// - Currently on one vCPU is supported. This code is written using threading
//   so adding support for more than one vCPU should be simple once the
//   hyperkernel supports this. Just create more vCPU threads
//
// - Currently this code doesn't handle when the VM wishes to go to sleep
//   by calling "hlt". Once the hyperkernel supports this, we will need to
//   add support for this application to wait on a kernel event. The only
//   way to wake up from "hlt" is to interrupt the CPU. Such and interrupt
//   will either have to come from an external interrupt, or it will have to
//   come from a timer interrupt. Either way, execution doesn't need to
//   continue until an event occurs, which we will have to add support for.
//
// - Currently, we do not support VMCS migration, which means we have to
//   set the affinity of bfexec. At some point, we need to implement
//   VMCS migration so that we can support executing from any core, at any
//   time.
//

#define alloc_page() platform_memset(platform_alloc_rwe(0x1000), 0, 0x1000);

/* -------------------------------------------------------------------------- */
/* VM                                                                         */
/* -------------------------------------------------------------------------- */

struct vm_t {
    struct crt_info_t crt_info;
    struct bfelf_loader_t bfelf_loader;
    struct bfelf_binary_t bfelf_binary;

    void *entry;

    uint64_t domainid;
    uint64_t vcpuid;

    FILE *file;
    pthread_t run_thread;
} g_vm;

/* -------------------------------------------------------------------------- */
/* Ack                                                                        */
/* -------------------------------------------------------------------------- */

uint32_t _cpuid_eax(uint32_t val) NOEXCEPT;

inline uint64_t
ack()
{ return _cpuid_eax(0xBF00); }

/* -------------------------------------------------------------------------- */
/* Signal Handling                                                            */
/* -------------------------------------------------------------------------- */

#include <signal.h>

void
kill_signal_handler(void)
{
    status_t ret;

    BFINFO("\n");
    BFINFO("\n");
    BFINFO("killing VM: %" PRId64 "\n", g_vm.domainid);

    ret = __vcpu_op__hlt_vcpu(g_vm.vcpuid);
    if (ret != SUCCESS) {
        BFALERT("__vcpu_op__hlt_vcpu failed\n");
        return;
    }

    return;
}

void
sig_handler(int sig)
{
    bfignored(sig);
    kill_signal_handler();
    return;
}

void
setup_kill_signal_handler(void)
{
    signal(SIGINT, sig_handler);
    signal(SIGQUIT, sig_handler);
    signal(SIGTERM, sig_handler);
}

/* -------------------------------------------------------------------------- */
/* Domain Functions                                                           */
/* -------------------------------------------------------------------------- */

status_t
domain_op__create_domain(void)
{
    g_vm.domainid = __domain_op__create_domain();
    if (g_vm.domainid == INVALID_DOMAINID) {
        BFALERT("__domain_op__create_domain failed\n");
        return FAILURE;
    }

    return SUCCESS;
}

status_t
domain_op__destroy_domain(void)
{
    status_t ret;

    ret = __domain_op__destroy_domain(g_vm.domainid);
    if (ret != SUCCESS) {
        BFALERT("__domain_op__destroy_domain failed\n");
        return FAILURE;
    }

    return SUCCESS;
}

status_t
domain_op__map_gpa(uint64_t gva, uint64_t gpa, uint64_t type)
{
    status_t ret;

    // TODO:
    //
    // We need to remove the use of mlock, and instead, a driver needs to
    // allocate non-paged memory for the guest, otherwise, the OS could
    // page the VM's memory out, which we cannot have, because the OS could
    // end up using the memory for something else.
    //

    if (mlock((void *)gva, 0x1000) != 0) {
        BFALERT("mlock failed: %s\n", strerror(errno));
        return FAILURE;
    }

    ret = __domain_op__map_gpa(g_vm.domainid, gva, gpa, type);
    if (ret != SUCCESS) {
        BFALERT("__domain_op__map_gpa failed\n");
        return FAILURE;
    }

    if (munlock((void *)gva, 0x1000) != 0) {
        BFALERT("munlock failed: %s\n", strerror(errno));
        return FAILURE;
    }

    return SUCCESS;
}

status_t
domain_op__map_buffer(
    uint64_t gva, uint64_t gpa, uint64_t size, uint64_t type)
{
    uint64_t index;

    for (index = 0; index < size; index += 0x1000) {
        status_t ret = domain_op__map_gpa(
            gva + index, gpa + index, type
        );

        if (ret != SUCCESS) {
            BFALERT("map_mem failed\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* vCPU Functions                                                             */
/* -------------------------------------------------------------------------- */

status_t
vcpu_op__create_vcpu(void)
{
    status_t ret;

    g_vm.vcpuid = __vcpu_op__create_vcpu(g_vm.domainid);
    if (g_vm.vcpuid == INVALID_VCPUID) {
        BFALERT("__vcpu_op__create_vcpu failed\n");
        return FAILURE;
    }

//
// REMOVE ME
//
g_vm.entry = (void *)0x1000370;
//
// REMOVE ME
//

    ret = __vcpu_op__set_rip(g_vm.vcpuid, (uint64_t)g_vm.entry);
    if (ret != SUCCESS) {
        BFALERT("__vcpu_op__set_rip failed\n");
        return FAILURE;
    }

    return SUCCESS;
}

void *
vcpu_op__run_vcpu(void *arg)
{
    status_t ret;
    bfignored(arg);

    while (1) {
        ret = __vcpu_op__run_vcpu(g_vm.vcpuid);
        switch(ret) {
            case SUCCESS:
                return 0;

            case FAILURE:
                BFALERT("__vcpu_op__run_vcpu failed\n");
                exit(-1);
                return 0;

            case VCPU_OP__RUN_CONTINUE:
                continue;

            default:
                BFALERT("unknown return code: \n", ret);
                return 0;
        }
    }
}

status_t
vcpu_op__destroy_vcpu(void)
{
    status_t ret;

    ret = __vcpu_op__destroy_vcpu(g_vm.vcpuid);
    if (ret != SUCCESS) {
        BFALERT("__vcpu_op__destroy_vcpu failed\n");
        return FAILURE;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Threading                                                                  */
/* -------------------------------------------------------------------------- */

#include <pthread.h>

void
start_run_thread()
{ pthread_create(&g_vm.run_thread, 0, vcpu_op__run_vcpu, 0); }

/* -------------------------------------------------------------------------- */
/* Memory Layout                                                              */
/* -------------------------------------------------------------------------- */

#define E820_MAP_SIZE 4

/**
 *       0x0 +----------------------+ ---
 *           | Unusable             |  | Unusable
 *    0x1000 +----------------------+ ---
 *           | Initial GDT          |  |
 *    0x2000 +----------------------+  |
 *           | Initial IDT          |  |
 *    0x3000 +----------------------+  |
 *           | Initial TSS          |  | Reserved
 *    0x4000 +----------------------+  |
 *           | Xen Start Info       |  |
 *    0x5000 +----------------------+  |
 *           | Xen CMD Line         |  |
 *    0x6000 +----------------------+ ---
 *           | Xen shared Info Page |  | RAM
 *    0x7000 +----------------------+ ---
 *           | Unusable             |  | Unusable
 *   0xE0000 +----------------------+ ---
 *           | ACPI                 |  | Reserved
 *   0xF0000 +----------------------+ ---
 *           | Local APIC           |  | Reserved
 *  0x100000 +----------------------+ ---
 *           | Unusable             |  | Unusable
 * 0x1000000 +----------------------+ ---
 *           | Linux ELF (Xen PVH)  |  |
 *       XXX +----------------------+  | RAM
 *           | Usable RAM           |  |
 * 0x9000000 +----------------------+ ---
 */

typedef union {
    hvm_start_info start_info;
    char pad[0x1000];
} reserved_4000_t;

typedef struct {
    char cmdline[0x1000];
} reserved_5000_t;

typedef struct {
    char shared_info_page[0x1000];
} reserved_6000_t;

reserved_4000_t *g_reserved_4000 = 0;   /* Xen start info */
reserved_5000_t *g_reserved_5000 = 0;   /* Xen cmdline */
reserved_6000_t *g_reserved_6000 = 0;   /* Xen shared info page */

uint64_t g_ram_addr = 0x1000000;
uint64_t g_ram_size = 0x8000000;

void *g_zero_page;

status_t
setup_e820_map()
{
    status_t ret;

    ret = __domain_op__add_e820_entry(
        g_vm.domainid,
        0,
        0x1000,
        XEN_HVM_MEMMAP_TYPE_UNUSABLE
    );

    if (ret != SUCCESS) {
        BFALERT("__domain_op__add_e820_entry failed\n");
        return FAILURE;
    }

    ret = __domain_op__add_e820_entry(
        g_vm.domainid,
        0x1000,
        0x5000,
        XEN_HVM_MEMMAP_TYPE_RESERVED
    );

    if (ret != SUCCESS) {
        BFALERT("__domain_op__add_e820_entry failed\n");
        return FAILURE;
    }

    ret = __domain_op__add_e820_entry(
        g_vm.domainid,
        0x6000,
        0x1000,
        XEN_HVM_MEMMAP_TYPE_RAM
    );

    if (ret != SUCCESS) {
        BFALERT("__domain_op__add_e820_entry failed\n");
        return FAILURE;
    }

    ret = __domain_op__add_e820_entry(
        g_vm.domainid,
        0x7000,
        g_ram_addr - 0x7000,
        XEN_HVM_MEMMAP_TYPE_UNUSABLE
    );

    if (ret != SUCCESS) {
        BFALERT("__domain_op__add_e820_entry failed\n");
        return FAILURE;
    }

    ret = __domain_op__add_e820_entry(
        g_vm.domainid,
        g_ram_addr,
        g_ram_size,
        XEN_HVM_MEMMAP_TYPE_RAM
    );

    if (ret != SUCCESS) {
        BFALERT("__domain_op__add_e820_entry failed\n");
        return FAILURE;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* ELF File Functions                                                         */
/* -------------------------------------------------------------------------- */

status_t
binary_read(const char *filename)
{
    char *data;
    uint64_t size;

    g_vm.file = fopen(filename, "rb");
    if (g_vm.file == 0) {
        BFALERT("failed to open: %s\n", filename);
        return FAILURE;
    }

    if (fseek(g_vm.file, 0, SEEK_END) != 0) {
        BFALERT("fseek failed: %s\n", strerror(errno));
        return FAILURE;
    }

    size = (uint64_t)ftell(g_vm.file);
    if (size == (uint64_t)-1) {
        BFALERT("ftell failed: %s\n", strerror(errno));
        return FAILURE;
    }

    if (fseek(g_vm.file, 0, SEEK_SET) != 0) {
        BFALERT("fseek failed: %s\n", strerror(errno));
        return FAILURE;
    }

    data = (char *)platform_alloc_rwe(size);
    if (data == 0) {
        BFALERT("malloc failed: %s\n", strerror(errno));
        return FAILURE;
    }

    if (fread(data, 1, size, g_vm.file) != size) {
        BFALERT("fread failed to read entire file: %s\n", strerror(errno));
        return FAILURE;
    }

    g_vm.bfelf_binary.file = data;
    g_vm.bfelf_binary.file_size = size;

    return SUCCESS;
}

status_t
binary_load(void)
{
    status_t ret;
    uint64_t gva;

    /**
     * NOTE:
     *
     * For PIE, we need to provide an address (g_ram_addr). This will be
     * overwritten if the binary is non-PIE (i.e. static), which is why we
     * have to get the start address again after we call bfelf_load
     *
     * NOTE:
     *
     * This is where we allocate RAM. We let the ELF loader allocate RAM for
     * us, and fill in the first part of RAM with the ELF file. The ELF
     * loader will ensure RAM is zero'd out, and will ensure the RAM is page
     * aligned, which is needed for mapping.
    */

    g_vm.bfelf_binary.exec_size = g_ram_size;
    g_vm.bfelf_binary.start_addr = (void *)g_ram_addr;

    ret = bfelf_load(&g_vm.bfelf_binary, 1, &g_vm.entry, &g_vm.crt_info, &g_vm.bfelf_loader);
    if (ret != BF_SUCCESS) {
        BFALERT("bfelf_load: 0x%016" PRIx64 "\n", ret);
        return FAILURE;
    }

    gva = (uint64_t)g_vm.bfelf_binary.exec;
    g_ram_addr = (uint64_t)g_vm.bfelf_binary.start_addr;

    ret = domain_op__map_buffer(gva, g_ram_addr, g_ram_size, MAP_RWE);
    if (ret != SUCCESS) {
        BFALERT("bfelf_load: 0x%016" PRIx64 "\n", ret);
        return FAILURE;
    }

    return SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Xen Info                                                                   */
/* -------------------------------------------------------------------------- */

status_t
setup_xen_start_info()
{
    status_t ret;

    g_reserved_4000 = (reserved_4000_t *)alloc_page();
    if (g_reserved_4000 == 0) {
        BFALERT("g_reserved_4000 alloc failed: %s\n", strerror(errno));
        return FAILURE;
    }

    g_reserved_4000->start_info.magic = XEN_HVM_START_MAGIC_VALUE;
    g_reserved_4000->start_info.version = 0;
    g_reserved_4000->start_info.cmdline_paddr = 0x5000;
    g_reserved_4000->start_info.rsdp_paddr = ACPI_RSDP_GPA;

    ret = domain_op__map_gpa((uint64_t)g_reserved_4000, 0x4000, MAP_RO);
    if (ret != BF_SUCCESS) {
        BFALERT("__domain_op__map_gpa failed\n");
        return FAILURE;
    }

    ret = __vcpu_op__set_rbx(g_vm.vcpuid, 0x4000);
    if (ret != SUCCESS) {
        BFALERT("__vcpu_op__set_rbx failed\n");
        return FAILURE;
    }

    return SUCCESS;
}

status_t
setup_xen_cmdline()
{
    status_t ret;
    const char *cmdline = "";

    /**
     * TODO:
     * - We need to use a "--" similar to gdb to get the command line options
     * from the user so that they can be added here
     */

    g_reserved_5000 = (reserved_5000_t *)alloc_page();
    if (g_reserved_5000 == 0) {
        BFALERT("g_reserved_5000 alloc failed: %s\n", strerror(errno));
        return FAILURE;
    }

    ret = domain_op__map_gpa((uint64_t)g_reserved_5000, 0x5000, MAP_RO);
    if (ret != BF_SUCCESS) {
        BFALERT("__domain_op__map_gpa failed\n");
        return FAILURE;
    }

    strcpy(g_reserved_5000->cmdline, cmdline);
    return SUCCESS;
}

status_t
setup_xen_shared_info_page()
{
    status_t ret;

    g_reserved_6000 = (reserved_6000_t *)alloc_page();
    if (g_reserved_6000 == 0) {
        BFALERT("g_reserved_6000 alloc failed: %s\n", strerror(errno));
        return FAILURE;
    }

    ret = domain_op__map_gpa((uint64_t)g_reserved_6000, 0x6000, MAP_RW);
    if (ret != BF_SUCCESS) {
        BFALERT("__domain_op__map_gpa failed\n");
        return FAILURE;
    }

    return SUCCESS;
}

status_t
setup_xen_disabled()
{
    status_t ret;

    /**
     * Note:
     *
     * The following disables specific portions of memory by mapping them to
     * a zero page. Specifically, the guest might attempt to access these
     * pages expecting to find something, which they will not. If we don't map
     * these, the guest will attempt to access them anyways and crash from an
     * EPT violation
     */

    g_zero_page = alloc_page();
    if (g_zero_page == 0) {
        BFALERT("g_zero_page alloc failed: %s\n", strerror(errno));
        return FAILURE;
    }

    /* Zero Page */
    ret = domain_op__map_buffer((uint64_t)g_zero_page, 0x0, 0x1000, MAP_RO);
    if (ret != BF_SUCCESS) {
        BFALERT("domain_op__map_buffer failed\n");
        return FAILURE;
    }

    /* Disable DMI */
    ret = domain_op__map_buffer((uint64_t)g_zero_page, 0xF0000, 0x10000, MAP_RO);
    if (ret != BF_SUCCESS) {
        BFALERT("domain_op__map_buffer failed\n");
        return FAILURE;
    }

    /* Disable Video BIOS region */
    ret = domain_op__map_buffer((uint64_t)g_zero_page, 0xC0000, 0x10000, MAP_RO);
    if (ret != BF_SUCCESS) {
        BFALERT("domain_op__map_buffer failed\n");
        return FAILURE;
    }

    /* ROMs */
    ret = domain_op__map_buffer((uint64_t)g_zero_page, 0xD0000, 0x10000, MAP_RO);
    if (ret != BF_SUCCESS) {
        BFALERT("domain_op__map_buffer failed\n");
        return FAILURE;
    }

    /* ROMs */
    ret = domain_op__map_buffer((uint64_t)g_zero_page, 0xE4000, 0x10000 - 0x4000, MAP_RO);
    if (ret != BF_SUCCESS) {
        BFALERT("domain_op__map_buffer failed\n");
        return FAILURE;
    }

    // TODO:
    //
    // To support MP, we will have to remove the following and provide an
    // actual MP table that mimics the ACPI tables as both are required.
    //
    // https://elixir.bootlin.com/linux/v3.7/source/arch/x86/kernel/mpparse.c#L604
    //

    /* MP Table */
    ret = domain_op__map_buffer((uint64_t)g_zero_page, 0x9F000, 0x1000, MAP_RO);
    if (ret != BF_SUCCESS) {
        BFALERT("domain_op__map_buffer failed\n");
        return FAILURE;
    }

    return SUCCESS;
}


/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */

int
main(int argc, const char *argv[])
{
    status_t ret;
    memset(&g_vm, 0, sizeof(g_vm));

    if (argc != 2) {
        BFALERT("invalid number of arguments\n");
        return EXIT_FAILURE;
    }

    if (ack() == 0) {
        return EXIT_FAILURE;
    }

    set_affinity(0);
    setup_kill_signal_handler();

    ret = domain_op__create_domain();
    if (ret != SUCCESS) {
        BFALERT("create_domain failed\n");
        return EXIT_FAILURE;
    }

    ret = setup_e820_map();
    if (ret != SUCCESS) {
        BFALERT("setup_e820_map failed\n");
        goto CLEANUP_VCPU;
    }

    ret = binary_read(argv[1]);
    if (ret != SUCCESS) {
        BFALERT("read_binary failed\n");
        goto CLEANUP_DOMAIN;
    }

    ret = binary_load();
    if (ret != SUCCESS) {
        BFALERT("load_binary failed\n");
        goto CLEANUP_DOMAIN;
    }

    ret = vcpu_op__create_vcpu();
    if (ret != SUCCESS) {
        BFALERT("create_vcpu failed\n");
        goto CLEANUP_DOMAIN;
    }

    ret = setup_xen_start_info();
    if (ret != SUCCESS) {
        BFALERT("setup_xen_start_info failed\n");
        goto CLEANUP_VCPU;
    }

    ret = setup_xen_cmdline();
    if (ret != SUCCESS) {
        BFALERT("setup_xen_cmdline failed\n");
        goto CLEANUP_VCPU;
    }

    ret = setup_xen_shared_info_page();
    if (ret != SUCCESS) {
        BFALERT("setup_xen_shared_info_page failed\n");
        goto CLEANUP_VCPU;
    }

    ret = setup_xen_disabled();
    if (ret != SUCCESS) {
        BFALERT("setup_xen_disabled failed\n");
        goto CLEANUP_VCPU;
    }

    start_run_thread();
    pthread_join(g_vm.run_thread, 0);

CLEANUP_VCPU:

    ret = vcpu_op__destroy_vcpu();
    if (ret != SUCCESS) {
        BFALERT("destroy_vcpu failed\n");
    }

CLEANUP_DOMAIN:

    ret = domain_op__destroy_domain();
    if (ret != SUCCESS) {
        BFALERT("destroy_domain failed\n");
    }

    return EXIT_SUCCESS;
}