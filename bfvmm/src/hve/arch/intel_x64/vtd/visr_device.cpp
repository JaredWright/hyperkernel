#include <bfvmm/hve/arch/intel_x64/exit_handler.h>
#include <eapis/hve/arch/intel_x64/vcpu.h>
#include <bfvmm/memory_manager/memory_manager.h>

#include "hve/arch/intel_x64/vtd/vtd_sandbox.h"

#include "hve/arch/intel_x64/vtd/ioapic.h"

namespace vtd_sandbox
{
namespace visr_device
{

using namespace eapis::intel_x64;

gsl::span<uint32_t> g_virtual_pci_config {};

// Initial PCI Configuration space for the emulated device
const uint32_t vendor_device = 0xBEEFF00D;      // Non-existent PCI Vendor/Device
const uint32_t status_command = 0x00000001;
const uint32_t class_sub_prog_rev = 0xFF000000; // Vendor-specific class
const uint32_t bist_htype_ltimer_clsize = 0;
const uint32_t bar0 = 0;
const uint32_t bar1 = 0;
const uint32_t bar2 = 0;
const uint32_t bar3 = 0;
const uint32_t bar4 = 0;
const uint32_t bar5 = 0;
const uint32_t cis_ptr = 0;
const uint32_t subid_subvendor = 0;
const uint32_t option_rom_bar = 0;
const uint32_t cap_ptr = 0;
const uint32_t lat_grant_pin_line = 0x100;

// The physical bus/device/function the emulated device will occupy
uint64_t g_bus = 0;
uint64_t g_device = 0;
uint64_t g_function = 0;

// The two interrupt vectors to be remapped to/from each other
static volatile uint64_t g_visr_vector = 0;
static volatile uint64_t g_ndvm_vector = 0;

bool
handle_cfc_in(
    gsl::not_null<vcpu_t *> vcpu,
    io_instruction_handler::info_t &info
)
{
    bfignored(vcpu);
    namespace io_instruction = vmcs_n::exit_qualification::io_instruction;

    auto cf8 = ::x64::portio::ind(0xCF8);
    auto reg_number = (cf8 & 0x000000FC) >> 2U;
    auto emulate_addr = 0x80000000U | (g_bus << 16U) | (g_device << 11U) | (g_function << 8U);
    auto next_addr = 0x80000000U | (g_bus << 16U) | (g_device << 11U) | ((g_function + 1) << 8U);

    if ((emulate_addr <= cf8) && (cf8 < next_addr)) {
        // bfdebug_nhex(0, "Read from emulated device register:", reg_number);
        auto emulated_val = g_virtual_pci_config.at(reg_number);
        switch (info.size_of_access) {
            case io_instruction::size_of_access::one_byte:
                emulated_val = emulated_val & 0xFF;
                // bfdebug_subnhex(0, "One byte in emulated from CFC:", emulated_val);
                info.val = emulated_val;
                break;

            case io_instruction::size_of_access::two_byte:
                emulated_val = emulated_val & 0xFFFF;
                // bfdebug_subnhex(0, "Two bytes in emulated from CFC:", emulated_val);
                info.val = emulated_val;
                break;

            default:
                // bfdebug_subnhex(0, "Four bytes in emulated from CFC:", emulated_val);
                info.val = emulated_val;
        }
        info.val = emulated_val;
    }
    else {
        switch (info.size_of_access) {
            case io_instruction::size_of_access::one_byte:
                info.val = ::x64::portio::inb(0xCFC);
                break;
            case io_instruction::size_of_access::two_byte:
                info.val = ::x64::portio::inw(0xCFC);
                break;
            default:
                info.val = ::x64::portio::ind(0xCFC);
        }
    }

    return true;
}

bool
handle_cfc_out(
    gsl::not_null<vcpu_t *> vcpu,
    io_instruction_handler::info_t &info
)
{
    bfignored(vcpu);
    namespace io_instruction = vmcs_n::exit_qualification::io_instruction;

    auto cf8 = ::x64::portio::ind(0xCF8);
    auto device_addr = cf8 & 0xFFFFFF00;
    auto reg_number = (cf8 & 0x000000FC) >> 2U;
    auto emulate_addr = 0x80000000U | (g_bus << 16U) | (g_device << 11U) | (g_function << 8U);

    if (device_addr == emulate_addr) {
        // bfdebug_nhex(0, "Write to emulated device register:", reg_number);
        auto val_written = info.val;
        auto old_val = g_virtual_pci_config.at(reg_number);
        switch (info.size_of_access) {
            case io_instruction::size_of_access::one_byte:
                val_written = val_written & 0xFF;
                g_virtual_pci_config.at(reg_number) =
                    (old_val & 0xFFFFFF00) | gsl::narrow_cast<uint32_t>(val_written);
                // bfdebug_subnhex(0, "One byte value written to CFC", val_written);
                // bfdebug_subnhex(0, "New register value", g_virtual_pci_config.at(reg_number));
                break;

            case io_instruction::size_of_access::two_byte:
                val_written = val_written & 0xFFFF;
                g_virtual_pci_config.at(reg_number) =
                    (old_val & 0xFFFF0000) | gsl::narrow_cast<uint32_t>(val_written);
                // bfdebug_subnhex(0, "Two byte value written to CFC", val_written);
                // bfdebug_subnhex(0, "New register value", g_virtual_pci_config.at(reg_number));
                break;

            default:
                g_virtual_pci_config.at(reg_number) =
                    gsl::narrow_cast<uint32_t>(val_written);
                // bfdebug_subnhex(0, "Four byte value written to CFC", val_written);
                // bfdebug_subnhex(0, "New register value", g_virtual_pci_config.at(reg_number));
        }
    }
    else {
        switch (info.size_of_access) {
            case io_instruction::size_of_access::one_byte:
                ::x64::portio::outb(0xCFC, gsl::narrow_cast<uint8_t>(info.val));
                break;
            case io_instruction::size_of_access::two_byte:
                ::x64::portio::outw(0xCFC, gsl::narrow_cast<uint16_t>(info.val));
                break;
            default:
                ::x64::portio::outd(0xCFC, gsl::narrow_cast<uint32_t>(info.val));
        }
    }

    return true;
}

bool
handle_cfd_in(
    gsl::not_null<vcpu_t *> vcpu,
    io_instruction_handler::info_t &info
)
{
    bfignored(vcpu);
    namespace io_instruction = vmcs_n::exit_qualification::io_instruction;

    auto cf8 = ::x64::portio::ind(0xCF8);
    auto reg_number = (cf8 & 0x000000FC) >> 2U;
    auto emulate_addr = 0x80000000U | (g_bus << 16U) | (g_device << 11U) | (g_function << 8U);
    auto next_addr = 0x80000000U | (g_bus << 16U) | (g_device << 11U) | ((g_function + 1) << 8U);

    if ((emulate_addr <= cf8) && (cf8 < next_addr)) {
        // bfdebug_nhex(0, "Read from emulated device register:", reg_number);
        auto emulated_val = (g_virtual_pci_config.at(reg_number)) >> 8;
        switch (info.size_of_access) {
            case io_instruction::size_of_access::one_byte:
                emulated_val = emulated_val & 0xFF;
                // bfdebug_subnhex(0, "One byte in emulated from CFD:", emulated_val);
                break;

            case io_instruction::size_of_access::two_byte:
                emulated_val = emulated_val & 0xFFFF;
                // bfdebug_subnhex(0, "Two byte in emulated from CFD:", emulated_val);
                break;

            default:
                // bfdebug_subnhex(0, "Four byte in emulated from CFD:", emulated_val);
                break;
        }
        info.val = emulated_val;
    }
    else {
        switch (info.size_of_access) {
            case io_instruction::size_of_access::one_byte:
                info.val = ::x64::portio::inb(0xCFD);
                break;
            case io_instruction::size_of_access::two_byte:
                info.val = ::x64::portio::inw(0xCFD);
                break;
            default:
                info.val = ::x64::portio::ind(0xCFD);
        }
    }

    return true;
}

bool
handle_cfd_out(
    gsl::not_null<vcpu_t *> vcpu,
    io_instruction_handler::info_t &info
)
{
    bfignored(vcpu);
    namespace io_instruction = vmcs_n::exit_qualification::io_instruction;

    switch (info.size_of_access) {
        case io_instruction::size_of_access::one_byte:
            ::x64::portio::outb(0xCFD, gsl::narrow_cast<uint8_t>(info.val));
            break;
        case io_instruction::size_of_access::two_byte:
            ::x64::portio::outw(0xCFD, gsl::narrow_cast<uint16_t>(info.val));
            break;
        default:
            ::x64::portio::outd(0xCFD, gsl::narrow_cast<uint32_t>(info.val));
    }

    return true;
}

bool
handle_cfe_in(
    gsl::not_null<vcpu_t *> vcpu,
    io_instruction_handler::info_t &info
)
{
    bfignored(vcpu);
    namespace io_instruction = vmcs_n::exit_qualification::io_instruction;

    auto cf8 = ::x64::portio::ind(0xCF8);
    auto reg_number = (cf8 & 0x000000FC) >> 2U;
    auto emulate_addr = 0x80000000U | (g_bus << 16U) | (g_device << 11U) | (g_function << 8U);
    auto next_addr = 0x80000000U | (g_bus << 16U) | (g_device << 11U) | ((g_function + 1) << 8U);

    if ((emulate_addr <= cf8) && (cf8 < next_addr)) {
        // bfdebug_nhex(0, "Read from emulated device register:", reg_number);
        auto emulated_val = (g_virtual_pci_config.at(reg_number)) >> 16;
        switch (info.size_of_access) {
            case io_instruction::size_of_access::one_byte:
                emulated_val = emulated_val & 0xFF;
                // bfdebug_subnhex(0, "One byte in emulated from CFE:", emulated_val);
                break;

            case io_instruction::size_of_access::two_byte:
                emulated_val = emulated_val & 0xFFFF;
                // bfdebug_subnhex(0, "Two byte in emulated from CFE:", emulated_val);
                break;

            default:
                // bfdebug_subnhex(0, "Four byte in emulated from CFE:", emulated_val);
                break;
        }
        info.val = emulated_val;
    }
    else {
        switch (info.size_of_access) {
            case io_instruction::size_of_access::one_byte:
                info.val = ::x64::portio::inb(0xCFE);
                break;
            case io_instruction::size_of_access::two_byte:
                info.val = ::x64::portio::inw(0xCFE);
                break;
            default:
                info.val = ::x64::portio::ind(0xCFE);
        }
    }

    return true;
}

bool
handle_cfe_out(
    gsl::not_null<vcpu_t *> vcpu,
    io_instruction_handler::info_t &info
)
{
    bfignored(vcpu);
    namespace io_instruction = vmcs_n::exit_qualification::io_instruction;

    switch (info.size_of_access) {
        case io_instruction::size_of_access::one_byte:
            ::x64::portio::outb(0xCFE, gsl::narrow_cast<uint8_t>(info.val));
            break;
        case io_instruction::size_of_access::two_byte:
            ::x64::portio::outw(0xCFE, gsl::narrow_cast<uint16_t>(info.val));
            break;
        default:
            ::x64::portio::outd(0xCFE, gsl::narrow_cast<uint32_t>(info.val));
    }

    return true;
}

bool
handle_cff_in(
    gsl::not_null<vcpu_t *> vcpu,
    io_instruction_handler::info_t &info
)
{
    bfignored(vcpu);
    namespace io_instruction = vmcs_n::exit_qualification::io_instruction;

    auto cf8 = ::x64::portio::ind(0xCF8);
    auto reg_number = (cf8 & 0x000000FC) >> 2U;
    auto emulate_addr = 0x80000000U | (g_bus << 16U) | (g_device << 11U) | (g_function << 8U);
    auto next_addr = 0x80000000U | (g_bus << 16U) | (g_device << 11U) | ((g_function + 1) << 8U);

    if ((emulate_addr <= cf8) && (cf8 < next_addr)) {
        // bfdebug_nhex(0, "Read from emulated device register:", reg_number);
        auto emulated_val = (g_virtual_pci_config.at(reg_number)) >> 24;
        switch (info.size_of_access) {
            case io_instruction::size_of_access::one_byte:
                emulated_val = emulated_val & 0xFF;
                // bfdebug_subnhex(0, "One byte in emulated from CFF:", emulated_val);
                break;

            case io_instruction::size_of_access::two_byte:
                emulated_val = emulated_val & 0xFFFF;
                // bfdebug_subnhex(0, "Two byte in emulated from CFF:", emulated_val);
                break;

            default:
                // bfdebug_subnhex(0, "Four byte in emulated from CFF:", emulated_val);
                break;
        }
        info.val = emulated_val;
    }
    else {
        switch (info.size_of_access) {
            case io_instruction::size_of_access::one_byte:
                info.val = ::x64::portio::inb(0xCFF);
                break;
            case io_instruction::size_of_access::two_byte:
                info.val = ::x64::portio::inw(0xCFF);
                break;
            default:
                info.val = ::x64::portio::ind(0xCFF);
        }
    }

    return true;
}

bool
handle_cff_out(
    gsl::not_null<vcpu_t *> vcpu,
    io_instruction_handler::info_t &info
)
{
    bfignored(vcpu);
    namespace io_instruction = vmcs_n::exit_qualification::io_instruction;

    switch (info.size_of_access) {
        case io_instruction::size_of_access::one_byte:
            ::x64::portio::outb(0xCFF, gsl::narrow_cast<uint8_t>(info.val));
            break;
        case io_instruction::size_of_access::two_byte:
            ::x64::portio::outw(0xCFF, gsl::narrow_cast<uint16_t>(info.val));
            break;
        default:
            ::x64::portio::outd(0xCFF, gsl::narrow_cast<uint32_t>(info.val));
    }

    return true;
}

void
print_ioapic_redirection_table(gsl::not_null<vcpu_t *> vcpu)
{
    bfignored(vcpu);

    // bfdebug_info(0, "Looking up IOAPIC Redirection Table");
    // uintptr_t ioapic_base_phys = 0xfec00000;
    // auto my_vcpu = vcpu_cast(vcpu);
    // auto ioapic_base_virt = my_vcpu->map_hpa_4k<uint32_t>(ioapic_base_phys);
    // uint32_t ioregsel = 0;
    // uint32_t iowin = 0;
    // intel_x64::ioapic::rte_t rte = 0;

    // ioregsel = 0;
    // ioapic_base_virt.get()[0] = ioregsel;
    // iowin = ioapic_base_virt.get()[4];
    // bfdebug_subnhex(0, "IOAPIC ID:", iowin);

    // ioregsel = 1;
    // ioapic_base_virt.get()[0] = ioregsel;
    // iowin = ioapic_base_virt.get()[4];
    // bfdebug_subnhex(0, "IOAPIC Version:", iowin);

    // ioregsel = 2;
    // ioapic_base_virt.get()[0] = ioregsel;
    // iowin = ioapic_base_virt.get()[4];
    // bfdebug_subnhex(0, "IOAPIC Arbitration ID:", iowin);

    // for(uint32_t i = 0x10; i <= 0x3F; i+=2) {
    //     // {31:0]
    //     ioregsel = i;
    //     ioapic_base_virt.get()[0] = ioregsel;
    //     iowin = ioapic_base_virt.get()[4];
    //     rte = iowin;

    //     // {63:32]
    //     ioregsel = i;
    //     ioapic_base_virt.get()[0] = ioregsel;
    //     iowin = ioapic_base_virt.get()[4];
    //     rte = rte | (iowin << 32);
    //     if(intel_x64::ioapic::rte::vector::get(rte) != 0xFF && rte != 0) {
    //         bfdebug_nhex(0, "IOAPIC Redirection Table Entry index:", i - 0x10);
    //         intel_x64::ioapic::rte::dump(0, rte);
    //         bfdebug_info(0, "");
    //     }
    // }
}

bool
receive_vector_from_ndvm(
    gsl::not_null<vcpu_t *> vcpu,
    cpuid_handler::info_t &info
)
{
    bfignored(vcpu);
    bfignored(info);

    g_ndvm_vector = vcpu->rcx();
    bfdebug_nhex(0, "Recieved vector from NDVM:", g_ndvm_vector);

    return true;
}

bool
receive_vector_from_windows(
    gsl::not_null<vcpu_t *> vcpu,
    cpuid_handler::info_t &info
)
{
    bfignored(vcpu);
    bfignored(info);

    g_visr_vector = vcpu->rcx();
    bfdebug_nhex(0, "Recieved vector from VISR driver:", g_visr_vector);
    print_ioapic_redirection_table(vcpu);

    return true;
}

bool
windows_test_interrupt_inject(
    gsl::not_null<vcpu_t *> vcpu,
    cpuid_handler::info_t &info
)
{
    bfignored(vcpu);
    bfignored(info);

    auto my_vcpu = vcpu_cast(vcpu);
    my_vcpu->queue_external_interrupt(g_visr_vector);
    bfdebug_info(0, "VISR driver requested an interrupt test, injecting to HDVM now");

    return true;
}

bool
windows_isr_ack(
    gsl::not_null<vcpu_t *> vcpu,
    cpuid_handler::info_t &info
)
{
    bfignored(vcpu);
    bfignored(info);

    bfdebug_info(0, "The VISR ran!");

    // TODO: Inject the interrupt into the NDVM
    // auto ndvm_vcpu = ?;
    // ndvm_vcpu->queue_external_interrupt(g_ndvm_vector);

    return true;
}

void
enable(
    gsl::not_null<eapis::intel_x64::vcpu *> vcpu,
    uint32_t bus,
    uint32_t device,
    uint32_t function
)
{
    // Make sure there is a real PCI device at the address we want to emulate
    uint32_t address = 0x80000000 | bus << 16 | device << 11 | function << 8;
    ::x64::portio::outd(0xCF8, address);
    auto data = ::x64::portio::ind(0xCFC);
    if(data == 0xFFFFFFFF) {
        bferror_info(0, "Failed to initalize Bareflank VISR device,");
        bferror_nhex(0, "A real PCI device must exist at IO address:", address);
        return;
    }

    g_bus = bus;
    g_device = device;
    g_function = function;

    g_virtual_pci_config = gsl::make_span(
        static_cast<uint32_t *>(alloc_page()),
        static_cast<long>(BAREFLANK_PAGE_SIZE / sizeof(uint32_t))
    );

    for(auto &val : g_virtual_pci_config) {
        val = 0xBADC0FFE;
    }

    g_virtual_pci_config.at(0) = vendor_device;
    g_virtual_pci_config.at(1) = status_command;
    g_virtual_pci_config.at(2) = class_sub_prog_rev;
    g_virtual_pci_config.at(3) = bist_htype_ltimer_clsize;
    g_virtual_pci_config.at(4) = bar0;
    g_virtual_pci_config.at(5) = bar1;
    g_virtual_pci_config.at(6) = bar2;
    g_virtual_pci_config.at(7) = bar3;
    g_virtual_pci_config.at(8) = bar4;
    g_virtual_pci_config.at(9) = bar5;
    g_virtual_pci_config.at(10) = cis_ptr;
    g_virtual_pci_config.at(11) = subid_subvendor;
    g_virtual_pci_config.at(12) = option_rom_bar;
    g_virtual_pci_config.at(13) = cap_ptr;
    g_virtual_pci_config.at(14) = 0;
    g_virtual_pci_config.at(15) = lat_grant_pin_line;

    // -------------------------------------------------------------------------
    // PCI configuration space handlers
    // -------------------------------------------------------------------------
    vcpu->add_io_instruction_handler(
        0xCFC,
        io_instruction_handler::handler_delegate_t::create <handle_cfc_in>(),
        io_instruction_handler::handler_delegate_t::create <handle_cfc_out>()
    );

    vcpu->add_io_instruction_handler(
        0xCFD,
        io_instruction_handler::handler_delegate_t::create <handle_cfd_in>(),
        io_instruction_handler::handler_delegate_t::create <handle_cfd_out>()
    );

    vcpu->add_io_instruction_handler(
        0xCFE,
        io_instruction_handler::handler_delegate_t::create <handle_cfe_in>(),
        io_instruction_handler::handler_delegate_t::create <handle_cfe_out>()
    );

    vcpu->add_io_instruction_handler(
        0xCFF,
        io_instruction_handler::handler_delegate_t::create <handle_cff_in>(),
        io_instruction_handler::handler_delegate_t::create <handle_cff_out>()
    );

    // -------------------------------------------------------------------------
    // Handlers to coordinate interupt injection
    // -------------------------------------------------------------------------
    vcpu->emulate_cpuid(
        0xd00dfeed,
        cpuid_handler::handler_delegate_t::create<receive_vector_from_ndvm>()
    );

    vcpu->emulate_cpuid(
        0xf00dbeef,
        cpuid_handler::handler_delegate_t::create<receive_vector_from_windows>()
    );

    vcpu->emulate_cpuid(
        0xdeadbeef,
        cpuid_handler::handler_delegate_t::create<windows_test_interrupt_inject>()
    );

    vcpu->emulate_cpuid(
        0xcafebabe,
        cpuid_handler::handler_delegate_t::create<windows_isr_ack>()
    );

    // bfdebug_info(0, "Virtual interrupt service route device initialized");
}

}
}
