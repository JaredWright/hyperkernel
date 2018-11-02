//
// Bareflank Hyperkernel
// Copyright (C) 2018 Assured Information Security, Inc.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include <hve/arch/intel_x64/vcpu.h>
#include <hve/arch/intel_x64/xen/evtchn_fifo.h>

// =============================================================================
// Implementation
// =============================================================================

namespace hyperkernel::intel_x64
{

evtchn_fifo::evtchn_fifo(gsl::not_null<vcpu *> vcpu)
:
    m_vcpu{vcpu}
{
    m_event_words.reserve(max_event_words_pages);
    m_event_chans.reserve(max_event_chans_pages);
}

void
evtchn_fifo::init_control(gsl::not_null<evtchn_init_control_t *> ctl)
{
    // TODO: We probably should bring down the whole system due to invalid
    // guest arguments. Xen returns error codes like -EINVAL in rax
    bfdebug_nhex(0, "ctl->vcpu", ctl->vcpu);
    bfdebug_nhex(0, "m_vcpu->id", m_vcpu->id());
//    expects(ctl->vcpu == m_vcpu->id()); the guest passes 0x10
    expects(ctl->offset <= (0x1000 - sizeof(evtchn_fifo_control_block_t)));
    expects(bfn::lower(ctl->offset, 3) == 0);

    ctl->link_bits = EVTCHN_FIFO_LINK_BITS;

    this->setup_control_block();
    this->map_control_block(ctl->control_gfn, ctl->offset);

    // set fifo port ops
    // setup ports
    //   -- struct evtchn
    //   -- port_is_valid
    //   -- evtchn_from_port
    //   -- is_bit_set shared_info
    //   -- set priority
}

void
evtchn_fifo::send(gsl::not_null<evtchn_send_t *> send)
{


}

// =============================================================================
// Internals
// =============================================================================

void
evtchn_fifo::setup_control_block()
{
    for (auto i = 0; i <= EVTCHN_FIFO_PRIORITY_MIN; i++) {
        m_queues[i].priority = i;
    }
}

void
evtchn_fifo::map_control_block(uint64_t gfn, uint32_t offset)
{
    m_ctl_blk_ump =
        m_vcpu->map_gpa_4k<uint8_t>(gfn << ::x64::pt::page_shift);

    if (!m_ctl_blk_ump) {
        throw std::runtime_error("map_gpa_4k failed");
    }

    uint8_t *base = m_ctl_blk_ump.get() + offset;
    m_ctl_blk = reinterpret_cast<evtchn_fifo_control_block_t *>(base);

    for (auto i = 0; i <= EVTCHN_FIFO_PRIORITY_MIN; i++) {
        m_queues[i].head = &m_ctl_blk->head[i];
    }
}

evtchn_fifo::word_t *
evtchn_fifo::word_from_port(port_t port)
{
    auto page_idx = port / event_words_per_page;
    auto word_idx = port % event_words_per_page;
    auto page_ptr = m_event_words.at(page_idx).get();

    return &page_ptr[word_idx];
}

event_word_t
evtchn_fifo::read_event_word(port_t port)
{
    auto word = this->word_from_port(port);
    return word->load();
}

void
evtchn_fifo::write_event_word(port_t port, event_word_t val)
{
    auto word = this->word_from_port(port);
    return word->store(val);
}

uint64_t
evtchn_fifo::nr_channels() const
{ return m_event_words.size() * event_words_per_page; }

bool
evtchn_fifo::port_is_valid(port_t p) const
{
    if (p >= max_nr_channels) {
        return 0;
    }
    return p < m_valid_channels.load();
}

bool evtchn_fifo::is_pending(word_t word) const
{ return is_bit_set(word.load(), EVTCHN_FIFO_PENDING); }

bool evtchn_fifo::is_masked(word_t word) const
{ return is_bit_set(word.load(), EVTCHN_FIFO_MASKED); }

bool evtchn_fifo::is_linked(word_t word) const
{ return is_bit_set(word.load(), EVTCHN_FIFO_LINKED); }

bool evtchn_fifo::is_busy(word_t word) const
{ return is_bit_set(word.load(), EVTCHN_FIFO_BUSY); }

//void evtchn_fifo::set_pending(word_t word)
//{ set_bit(word, EVTCHN_FIFO_PENDING); }
//
//void evtchn_fifo::set_masked(word_t word)
//{ set_bit(word, EVTCHN_FIFO_MASKED); }
//
//void evtchn_fifo::set_linked(word_t word)
//{ set_bit(word, EVTCHN_FIFO_LINKED); }
//
//void evtchn_fifo::set_busy(word_t word)
//{ set_bit(word, EVTCHN_FIFO_BUSY); }
//
//void evtchn_fifo::clear_pending(word_t word)
//{ clear_bit(word, EVTCHN_FIFO_PENDING); }
//
//void evtchn_fifo::clear_masked(word_t word)
//{ clear_bit(word, EVTCHN_FIFO_MASKED); }
//
//void evtchn_fifo::clear_linked(word_t word)
//{ clear_bit(word, EVTCHN_FIFO_LINKED); }
//
//void evtchn_fifo::clear_busy(word_t word)
//{ clear_bit(word, EVTCHN_FIFO_BUSY); }

}
