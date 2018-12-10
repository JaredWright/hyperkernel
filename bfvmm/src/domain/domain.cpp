//
// Bareflank Extended APIs
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

#include <bfdebug.h>
#include <domain/domain.h>

namespace hyperkernel
{

domain::domain(domainid_type domainid) :
    m_id{domainid}
{ }

void
domain::run(bfobject *obj)
{ bfignored(obj); }

void
domain::hlt(bfobject *obj)
{ bfignored(obj); }

void
domain::init(bfobject *obj)
{ bfignored(obj); }

void
domain::fini(bfobject *obj)
{ bfignored(obj); }

domain::domainid_type
domain::id() const noexcept
{ return m_id; }

void
domain::set_entry(uint64_t gpa) noexcept
{ m_entry = gpa; }

uint64_t
domain::entry() const noexcept
{ return m_entry; }

}
