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

#include <bfgsl.h>

#include <string.h>
#include <errno.h>

#include <hve/arch/intel_x64/vcpu.h>
#include <hve/arch/intel_x64/xen/evtchn_fifo.h>

// =============================================================================
// Implementation
// =============================================================================

namespace hyperkernel::intel_x64
{

evtchn_fifo::evtchn_fifo(
    gsl::not_null<vcpu *> vcpu,
    gsl::not_null<xen_op_handler *> handler)
:
    m_vcpu{vcpu},
    m_xen_handler{handler}
{
    m_event_word.reserve(event_word_capacity);
    m_event_group.reserve(event_group_capacity);

    this->make_bucket(0);
    auto chan = this->port_to_chan(0);
    chan->set_state(evtchn::reserved);

    bfdebug_nhex(0, "chan_capacity", chan_capacity);
    bfdebug_nhex(0, "word_per_page", word_per_page);
    bfdebug_nhex(0, "event_word_capacity", event_word_capacity);

    bfdebug_nhex(0, "bucket_per_group", bucket_per_group);
    bfdebug_nhex(0, "chan_per_bucket", chan_per_bucket);
    bfdebug_nhex(0, "chan_per_group", chan_per_group);
    bfdebug_nhex(0, "event_group_capacity", event_group_capacity);

    bfdebug_nhex(0, "evtchn_size", evtchn_size);
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
    this->setup_ports();
}

void
evtchn_fifo::send(gsl::not_null<evtchn_send_t *> send)
{
    auto port = send->port;

    if (!this->port_is_valid(port)) {
        throw std::invalid_argument("send: invalid port: " +
                                    std::to_string(port));
    }

//    auto chan = this->port_to_chan(port);
}

// =============================================================================
// Internals
// =============================================================================

evtchn_port_t
evtchn_fifo::make_new_port()
{
    for (evtchn_port_t p = 1; p < chan_capacity; p++) {
        if (this->make_port(p) == -EBUSY) {
            continue;
        }
        return p;
    }

    return 0;
}

int
evtchn_fifo::make_port(evtchn_port_t port)
{
    if (port >= chan_capacity) {
        throw std::invalid_argument("make_port: invalid_port: " +
                                    std::to_string(port));
    }

    if (this->port_is_valid(port)) {
        auto chan = this->port_to_chan(port);
        if (chan->state() != evtchn::free) {
            return -EBUSY;
        }

        if (this->port_is_busy(port)) {
            return -EBUSY;
        }

        return 0;
    }

    auto group = port / chan_per_group;
    if (group == m_event_group.size()) {
        this->make_bucket(port);
    }

    return 0;
}

void
evtchn_fifo::make_bucket(evtchn_port_t port)
{
    const auto cur = m_event_group.size();
    const auto cap = m_event_group.capacity();

    if (GSL_UNLIKELY(cur == cap)) {
        throw std::runtime_error("evtchn_fifo: out of buckets");
    }

    auto bucket = make_page<chan_t>();
    auto base = bucket.get();
    uint8_t *page = reinterpret_cast<uint8_t *>(bucket.get());
    memset(page, 0, ::x64::pt::page_size);

    for (auto i = 0; i < chan_per_bucket; i++) {
        auto chan = &base[i];
        chan->set_port(port + i);
        chan->set_priority(EVTCHN_FIFO_PRIORITY_DEFAULT);
    }

    m_event_group.push_back(std::move(bucket));
    m_valid_chans += chan_per_bucket;
}

void
evtchn_fifo::setup_ports()
{
    for (auto p = 1; p < chan_capacity; p++) {
        if (!this->port_is_valid(p)) {
            bfdebug_ndec(0, "setup_ports: invalid port: ", p)
            return;
        }

        auto arr = m_xen_handler->shared_info()->evtchn_pending;
        auto idx = p / bits_per_xen_ulong;
        auto bit = p % bits_per_xen_ulong;
        auto val = arr[idx];

        if (is_bit_set(bit, val)) {
            auto chan = this->port_to_chan(p);
            chan->set_pending();
            bfalert_nhex(0, "pending event at port:", p);
        }
    }
}

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

evtchn_fifo::chan_t *
evtchn_fifo::port_to_chan(port_t port) const
{
    auto grp_idx = port / chan_per_group;
    auto bkt_idx = port % chan_per_group / chan_per_bucket;
    auto bkt_ptr = m_event_group.at(grp_idx).get();

    if (bkt_idx >= chan_per_bucket) {
        throw std::invalid_argument("port_to_chan: port out of range: " +
                                    std::to_string(port));
    }

    auto chan = &bkt_ptr[bkt_idx];

    bfdebug_nhex(0, "port_to_chan: ", port);
    bfdebug_subnhex(0, "state: ", chan->state());
    bfdebug_subbool(0, "pending: ", chan->is_pending());
    bfdebug_subnhex(0, "priority: ", chan->priority());
    bfdebug_subnhex(0, "port: ", chan->port());

    return chan;
}

evtchn_fifo::word_t *
evtchn_fifo::port_to_word(port_t port) const
{
    auto page_idx = port / word_per_page;
    auto word_idx = port % word_per_page;
    auto page_ptr = m_event_word.at(page_idx).get();

    if (word_idx >= word_per_page) {
        throw std::invalid_argument("port_to_word: port out of range: " +
                                    std::to_string(port));
    }

    return &page_ptr[word_idx];
}

uint64_t
evtchn_fifo::word_count() const
{ return m_event_word.size() * word_per_page; }

uint64_t
evtchn_fifo::chan_count() const
{ return m_valid_chans.load(); }

bool evtchn_fifo::port_is_valid(port_t port) const
{ return port < chan_count(); }

bool evtchn_fifo::port_is_pending(port_t port) const
{
    auto word = this->port_to_word(port);
    return is_bit_set(word->load(), EVTCHN_FIFO_PENDING);
}

bool evtchn_fifo::port_is_masked(port_t port) const
{
    auto word = this->port_to_word(port);
    return is_bit_set(word->load(), EVTCHN_FIFO_MASKED); }

bool evtchn_fifo::port_is_linked(port_t port) const
{
    auto word = this->port_to_word(port);
    return is_bit_set(word->load(), EVTCHN_FIFO_LINKED);
}

bool evtchn_fifo::port_is_busy(port_t port) const
{
    auto word = this->port_to_word(port);
    return is_bit_set(word->load(), EVTCHN_FIFO_BUSY);
}

void evtchn_fifo::port_set_pending(port_t port)
{
    auto word = this->port_to_word(port);
    word->fetch_or(1U << EVTCHN_FIFO_PENDING);
}

void evtchn_fifo::port_set_masked(port_t port)
{
    auto word = this->port_to_word(port);
    word->fetch_or(1U << EVTCHN_FIFO_MASKED);
}

void evtchn_fifo::port_set_linked(port_t port)
{
    auto word = this->port_to_word(port);
    word->fetch_or(1U << EVTCHN_FIFO_LINKED);
}

void evtchn_fifo::port_set_busy(port_t port)
{
    auto word = this->port_to_word(port);
    word->fetch_or(1U << EVTCHN_FIFO_BUSY);
}

void evtchn_fifo::port_clear_pending(port_t port)
{
    auto word = this->port_to_word(port);
    word->fetch_and(~(1U << EVTCHN_FIFO_PENDING));
}

void evtchn_fifo::port_clear_masked(port_t port)
{
    auto word = this->port_to_word(port);
    word->fetch_and(~(1U << EVTCHN_FIFO_MASKED));
}

void evtchn_fifo::port_clear_linked(port_t port)
{
    auto word = this->port_to_word(port);
    word->fetch_and(~(1U << EVTCHN_FIFO_LINKED));
}

void evtchn_fifo::port_clear_busy(port_t port)
{
    auto word = this->port_to_word(port);
    word->fetch_and(~(1U << EVTCHN_FIFO_BUSY));
}

}
