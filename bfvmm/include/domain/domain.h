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

#ifndef DOMAIN_HYPERKERNEL_H
#define DOMAIN_HYPERKERNEL_H

#include <bftypes.h>
#include <bfobject.h>
#include <bfhypercall.h>

// -----------------------------------------------------------------------------
// Exports
// -----------------------------------------------------------------------------

#include <bfexports.h>

#ifndef STATIC_HYPERKERNEL_DOMAIN
#ifdef SHARED_HYPERKERNEL_DOMAIN
#define EXPORT_HYPERKERNEL_DOMAIN EXPORT_SYM
#else
#define EXPORT_HYPERKERNEL_DOMAIN IMPORT_SYM
#endif
#else
#define EXPORT_HYPERKERNEL_DOMAIN
#endif

// -----------------------------------------------------------------------------
// Definitions
// -----------------------------------------------------------------------------

namespace hyperkernel
{

/// Domain
///
class EXPORT_HYPERKERNEL_DOMAIN domain : public bfobject
{
public:

    using domainid_type = domainid_t;

public:

    /// Constructor
    ///
    /// @expects
    /// @ensures
    ///
    domain(domainid_type domainid);

    /// Destructor
    ///
    /// @expects
    /// @ensures
    ///
    virtual ~domain() = default;

    /// Run
    ///
    /// @expects none
    /// @ensures none
    ///
    /// @param obj object that can be passed around as needed
    ///     by extensions of Bareflank
    ///
    VIRTUAL void run(bfobject *obj = nullptr);

    /// Halt
    ///
    /// @expects none
    /// @ensures none
    ///
    /// @param obj object that can be passed around as needed
    ///     by extensions of Bareflank
    ///
    VIRTUAL void hlt(bfobject *obj = nullptr);

    /// Init vCPU
    ///
    /// @expects none
    /// @ensures none
    ///
    /// @param obj object that can be passed around as needed
    ///     by extensions of Bareflank
    ///
    VIRTUAL void init(bfobject *obj = nullptr);

    /// Fini vCPU
    ///
    /// @expects none
    /// @ensures none
    ///
    /// @param obj object that can be passed around as needed
    ///     by extensions of Bareflank
    ///
    VIRTUAL void fini(bfobject *obj = nullptr);

    /// Get ID
    ///
    /// @expects
    /// @ensures
    ///
    /// @return Returns the domain ID
    ///
    domainid_type id() const noexcept;

    /// Generate Domain ID
    ///
    /// Note:
    ///
    /// Xen has a max ID of 0x7FEF, which means that we would have
    /// a similar upper limit. For now this is not an issue, but if we
    /// get to a point where we support a large number of really small
    /// VMs, we could hit this limit and will need to address this.
    ///
    /// @expects
    /// @ensures
    ///
    /// @return Returns a new, unique domain id
    ///
    static domainid_type generate_domainid() noexcept
    {
        static domainid_type s_id = 1;
        return s_id++;
    }

    /// Set Entry
    ///
    /// Sets the entry point (GPA) of the VM. This can be usd by a vCPU
    /// to set it's entry point.
    ///
    /// @expects
    /// @ensures
    ///
    /// @param gpa the guest physical address of the VM's entry point
    ///
    void set_entry(uintptr_t gpa) noexcept;

    /// Get Entry
    ///
    /// @expects
    /// @ensures
    ///
    /// @return returns the VM's entry point
    ///
    uintptr_t entry() const noexcept;

private:

    domainid_type m_id;
    uintptr_t m_entry;

public:

    /// @cond

    domain(domain &&) = default;
    domain &operator=(domain &&) = default;

    domain(const domain &) = delete;
    domain &operator=(const domain &) = delete;

    /// @endcond
};

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

constexpr domain::domainid_type invalid_domainid = INVALID_DOMAINID;
constexpr domain::domainid_type self = SELF;

}

#endif
