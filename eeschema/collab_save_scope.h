/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef EESCHEMA_COLLAB_SAVE_SCOPE_H
#define EESCHEMA_COLLAB_SAVE_SCOPE_H

/**
 * Owns, or explicitly borrows, the native projection cut used by an eeschema
 * save.  A project save owns one scope for its complete multi-file operation;
 * saveSchematicFile() borrows that exact scope for each child writer instead
 * of attempting a nested acquisition.
 *
 * The callback indirection keeps the lifetime rule independently testable and
 * keeps this header free of Emscripten dependencies.
 */
class EESCHEMA_COLLAB_SAVE_SCOPE
{
public:
    using ACQUIRE_FN = bool (*)();
    using RELEASE_FN = void (*)();

    EESCHEMA_COLLAB_SAVE_SCOPE( const EESCHEMA_COLLAB_SAVE_SCOPE* aOuterScope,
                                ACQUIRE_FN aAcquire, RELEASE_FN aRelease ) noexcept :
            m_release( nullptr ),
            m_acquired( false ),
            m_ownsLease( false )
    {
        if( aOuterScope )
        {
            // Borrowing is valid only while the caller's concrete scope is
            // alive and valid.  The pointer makes that relationship explicit;
            // unlike a boolean "already held" flag it cannot silently invent
            // a lease.
            m_acquired = static_cast<bool>( *aOuterScope );
            return;
        }

        if( aAcquire && aRelease && aAcquire() )
        {
            m_release = aRelease;
            m_acquired = true;
            m_ownsLease = true;
        }
    }

    ~EESCHEMA_COLLAB_SAVE_SCOPE()
    {
        if( m_ownsLease )
            m_release();
    }

    EESCHEMA_COLLAB_SAVE_SCOPE( const EESCHEMA_COLLAB_SAVE_SCOPE& ) = delete;
    EESCHEMA_COLLAB_SAVE_SCOPE& operator=( const EESCHEMA_COLLAB_SAVE_SCOPE& ) = delete;

    explicit operator bool() const noexcept { return m_acquired; }
    bool ownsLease() const noexcept { return m_ownsLease; }

private:
    RELEASE_FN m_release;
    bool       m_acquired;
    bool       m_ownsLease;
};

#endif // EESCHEMA_COLLAB_SAVE_SCOPE_H
