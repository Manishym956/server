/*****************************************************************************

Copyright (c) 2007, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/lock0priv.h
Lock module internal structures and methods.

Created July 12, 2007 Vasil Dimov
*******************************************************/

#ifndef lock0priv_h
#define lock0priv_h

#ifndef LOCK_MODULE_IMPLEMENTATION
/* If you need to access members of the structures defined in this
file, please write appropriate functions that retrieve them and put
those functions in lock/ */
#error Do not include lock0priv.h outside of the lock/ module
#endif

#include "hash0hash.h"
#include "rem0types.h"
#include "trx0trx.h"

#ifndef UINT32_MAX
#define UINT32_MAX             (4294967295U)
#endif

/** Print the table lock into the given output stream
@param[in,out]	out	the output stream
@return the given output stream. */
inline
std::ostream& lock_table_t::print(std::ostream& out) const
{
	out << "[lock_table_t: name=" << table->name << "]";
	return(out);
}

/** The global output operator is overloaded to conveniently
print the lock_table_t object into the given output stream.
@param[in,out]	out	the output stream
@param[in]	lock	the table lock
@return the given output stream */
inline
std::ostream&
operator<<(std::ostream& out, const lock_table_t& lock)
{
	return(lock.print(out));
}

inline
std::ostream&
ib_lock_t::print(std::ostream& out) const
{
  static_assert(LOCK_MODE_MASK == 7, "compatibility");
  static_assert(LOCK_IS == 0, "compatibility");
  static_assert(LOCK_IX == 1, "compatibility");
  static_assert(LOCK_S == 2, "compatibility");
  static_assert(LOCK_X == 3, "compatibility");
  static_assert(LOCK_AUTO_INC == 4, "compatibility");
  static_assert(LOCK_NONE == 5, "compatibility");
  static_assert(LOCK_NONE_UNSET == 7, "compatibility");
  const char *const modes[8]=
  { "IS", "IX", "S", "X", "AUTO_INC", "NONE", "?", "NONE_UNSET" };

  out << "[lock_t: type_mode=" << type_mode << "(" << type_string()
      << " | LOCK_" << modes[mode()];

  if (is_record_not_gap())
    out << " | LOCK_REC_NOT_GAP";
  if (is_waiting())
    out << " | LOCK_WAIT";

  if (is_gap())
    out << " | LOCK_GAP";

  if (is_insert_intention())
    out << " | LOCK_INSERT_INTENTION";

  out << ")";

  if (is_table())
    out << un_member.tab_lock;
  else
    out << un_member.rec_lock;

  out << "]";
  return out;
}

inline
std::ostream&
operator<<(std::ostream& out, const ib_lock_t& lock)
{
	return(lock.print(out));
}

#ifdef UNIV_DEBUG
extern ibool	lock_print_waits;
#endif /* UNIV_DEBUG */

/* An explicit record lock affects both the record and the gap before it.
An implicit x-lock does not affect the gap, it only locks the index
record from read or update.

If a transaction has modified or inserted an index record, then
it owns an implicit x-lock on the record. On a secondary index record,
a transaction has an implicit x-lock also if it has modified the
clustered index record, the max trx id of the page where the secondary
index record resides is >= trx id of the transaction (or database recovery
is running), and there are no explicit non-gap lock requests on the
secondary index record.

This complicated definition for a secondary index comes from the
implementation: we want to be able to determine if a secondary index
record has an implicit x-lock, just by looking at the present clustered
index record, not at the historical versions of the record. The
complicated definition can be explained to the user so that there is
nondeterminism in the access path when a query is answered: we may,
or may not, access the clustered index record and thus may, or may not,
bump into an x-lock set there.

Different transaction can have conflicting locks set on the gap at the
same time. The locks on the gap are purely inhibitive: an insert cannot
be made, or a select cursor may have to wait if a different transaction
has a conflicting lock on the gap. An x-lock on the gap does not give
the right to insert into the gap.

An explicit lock can be placed on a user record or the supremum record of
a page. The locks on the supremum record are always thought to be of the gap
type, though the gap bit is not set. When we perform an update of a record
where the size of the record changes, we may temporarily store its explicit
locks on the infimum record of the page, though the infimum otherwise never
carries locks.

A waiting record lock can also be of the gap type. A waiting lock request
can be granted when there is no conflicting mode lock request by another
transaction ahead of it in the explicit lock queue.

In version 4.0.5 we added yet another explicit lock type: LOCK_REC_NOT_GAP.
It only locks the record it is placed on, not the gap before the record.
This lock type is necessary to emulate an Oracle-like READ COMMITTED isolation
level.

-------------------------------------------------------------------------
RULE 1: If there is an implicit x-lock on a record, and there are non-gap
-------
lock requests waiting in the queue, then the transaction holding the implicit
x-lock also has an explicit non-gap record x-lock. Therefore, as locks are
released, we can grant locks to waiting lock requests purely by looking at
the explicit lock requests in the queue.

RULE 3: Different transactions cannot have conflicting granted non-gap locks
-------
on a record at the same time. However, they can have conflicting granted gap
locks.
RULE 4: If a there is a waiting lock request in a queue, no lock request,
-------
gap or not, can be inserted ahead of it in the queue. In record deletes
and page splits new gap type locks can be created by the database manager
for a transaction, and without rule 4, the waits-for graph of transactions
might become cyclic without the database noticing it, as the deadlock check
is only performed when a transaction itself requests a lock!
-------------------------------------------------------------------------

An insert is allowed to a gap if there are no explicit lock requests by
other transactions on the next record. It does not matter if these lock
requests are granted or waiting, gap bit set or not, with the exception
that a gap type request set by another transaction to wait for
its turn to do an insert is ignored. On the other hand, an
implicit x-lock by another transaction does not prevent an insert, which
allows for more concurrency when using an Oracle-style sequence number
generator for the primary key with many transactions doing inserts
concurrently.

A modify of a record is allowed if the transaction has an x-lock on the
record, or if other transactions do not have any non-gap lock requests on the
record.

A read of a single user record with a cursor is allowed if the transaction
has a non-gap explicit, or an implicit lock on the record, or if the other
transactions have no x-lock requests on the record. At a page supremum a
read is always allowed.

In summary, an implicit lock is seen as a granted x-lock only on the
record, not on the gap. An explicit lock with no gap bit set is a lock
both on the record and the gap. If the gap bit is set, the lock is only
on the gap. Different transaction cannot own conflicting locks on the
record at the same time, but they may own conflicting locks on the gap.
Granted locks on a record give an access right to the record, but gap type
locks just inhibit operations.

NOTE: Finding out if some transaction has an implicit x-lock on a secondary
index record can be cumbersome. We may have to look at previous versions of
the corresponding clustered index record to find out if a delete marked
secondary index record was delete marked by an active transaction, not by
a committed one.

FACT A: If a transaction has inserted a row, it can delete it any time
without need to wait for locks.

PROOF: The transaction has an implicit x-lock on every index record inserted
for the row, and can thus modify each record without the need to wait. Q.E.D.

FACT B: If a transaction has read some result set with a cursor, it can read
it again, and retrieves the same result set, if it has not modified the
result set in the meantime. Hence, there is no phantom problem. If the
biggest record, in the alphabetical order, touched by the cursor is removed,
a lock wait may occur, otherwise not.

PROOF: When a read cursor proceeds, it sets an s-lock on each user record
it passes, and a gap type s-lock on each page supremum. The cursor must
wait until it has these locks granted. Then no other transaction can
have a granted x-lock on any of the user records, and therefore cannot
modify the user records. Neither can any other transaction insert into
the gaps which were passed over by the cursor. Page splits and merges,
and removal of obsolete versions of records do not affect this, because
when a user record or a page supremum is removed, the next record inherits
its locks as gap type locks, and therefore blocks inserts to the same gap.
Also, if a page supremum is inserted, it inherits its locks from the successor
record. When the cursor is positioned again at the start of the result set,
the records it will touch on its course are either records it touched
during the last pass or new inserted page supremums. It can immediately
access all these records, and when it arrives at the biggest record, it
notices that the result set is complete. If the biggest record was removed,
lock wait can occur because the next record only inherits a gap type lock,
and a wait may be needed. Q.E.D. */

/* If an index record should be changed or a new inserted, we must check
the lock on the record or the next. When a read cursor starts reading,
we will set a record level s-lock on each record it passes, except on the
initial record on which the cursor is positioned before we start to fetch
records. Our index tree search has the convention that the B-tree
cursor is positioned BEFORE the first possibly matching record in
the search. Optimizations are possible here: if the record is searched
on an equality condition to a unique key, we could actually set a special
lock on the record, a lock which would not prevent any insert before
this record. In the next key locking an x-lock set on a record also
prevents inserts just before that record.
	There are special infimum and supremum records on each page.
A supremum record can be locked by a read cursor. This records cannot be
updated but the lock prevents insert of a user record to the end of
the page.
	Next key locks will prevent the phantom problem where new rows
could appear to SELECT result sets after the select operation has been
performed. Prevention of phantoms ensures the serializability of
transactions.
	What should we check if an insert of a new record is wanted?
Only the lock on the next record on the same page, because also the
supremum record can carry a lock. An s-lock prevents insertion, but
what about an x-lock? If it was set by a searched update, then there
is implicitly an s-lock, too, and the insert should be prevented.
What if our transaction owns an x-lock to the next record, but there is
a waiting s-lock request on the next record? If this s-lock was placed
by a read cursor moving in the ascending order in the index, we cannot
do the insert immediately, because when we finally commit our transaction,
the read cursor should see also the new inserted record. So we should
move the read cursor backward from the next record for it to pass over
the new inserted record. This move backward may be too cumbersome to
implement. If we in this situation just enqueue a second x-lock request
for our transaction on the next record, then the deadlock mechanism
notices a deadlock between our transaction and the s-lock request
transaction. This seems to be an ok solution.
	We could have the convention that granted explicit record locks,
lock the corresponding records from changing, and also lock the gaps
before them from inserting. A waiting explicit lock request locks the gap
before from inserting. Implicit record x-locks, which we derive from the
transaction id in the clustered index record, only lock the record itself
from modification, not the gap before it from inserting.
	How should we store update locks? If the search is done by a unique
key, we could just modify the record trx id. Otherwise, we could put a record
x-lock on the record. If the update changes ordering fields of the
clustered index record, the inserted new record needs no record lock in
lock table, the trx id is enough. The same holds for a secondary index
record. Searched delete is similar to update.

PROBLEM:
What about waiting lock requests? If a transaction is waiting to make an
update to a record which another modified, how does the other transaction
know to send the end-lock-wait signal to the waiting transaction? If we have
the convention that a transaction may wait for just one lock at a time, how
do we preserve it if lock wait ends?

PROBLEM:
Checking the trx id label of a secondary index record. In the case of a
modification, not an insert, is this necessary? A secondary index record
is modified only by setting or resetting its deleted flag. A secondary index
record contains fields to uniquely determine the corresponding clustered
index record. A secondary index record is therefore only modified if we
also modify the clustered index record, and the trx id checking is done
on the clustered index record, before we come to modify the secondary index
record. So, in the case of delete marking or unmarking a secondary index
record, we do not have to care about trx ids, only the locks in the lock
table must be checked. In the case of a select from a secondary index, the
trx id is relevant, and in this case we may have to search the clustered
index record.

PROBLEM: How to update record locks when page is split or merged, or
--------------------------------------------------------------------
a record is deleted or updated?
If the size of fields in a record changes, we perform the update by
a delete followed by an insert. How can we retain the locks set or
waiting on the record? Because a record lock is indexed in the bitmap
by the heap number of the record, when we remove the record from the
record list, it is possible still to keep the lock bits. If the page
is reorganized, we could make a table of old and new heap numbers,
and permute the bitmaps in the locks accordingly. We can add to the
table a row telling where the updated record ended. If the update does
not require a reorganization of the page, we can simply move the lock
bits for the updated record to the position determined by its new heap
number (we may have to allocate a new lock, if we run out of the bitmap
in the old one).
	A more complicated case is the one where the reinsertion of the
updated record is done pessimistically, because the structure of the
tree may change.

PROBLEM: If a supremum record is removed in a page merge, or a record
---------------------------------------------------------------------
removed in a purge, what to do to the waiting lock requests? In a split to
the right, we just move the lock requests to the new supremum. If a record
is removed, we could move the waiting lock request to its inheritor, the
next record in the index. But, the next record may already have lock
requests on its own queue. A new deadlock check should be made then. Maybe
it is easier just to release the waiting transactions. They can then enqueue
new lock requests on appropriate records.

PROBLEM: When a record is inserted, what locks should it inherit from the
-------------------------------------------------------------------------
upper neighbor? An insert of a new supremum record in a page split is
always possible, but an insert of a new user record requires that the upper
neighbor does not have any lock requests by other transactions, granted or
waiting, in its lock queue. Solution: We can copy the locks as gap type
locks, so that also the waiting locks are transformed to granted gap type
locks on the inserted record. */

/* LOCK COMPATIBILITY MATRIX
 *    IS IX S  X  AI
 * IS +	 +  +  -  +
 * IX +	 +  -  -  +
 * S  +	 -  +  -  -
 * X  -	 -  -  -  -
 * AI +	 +  -  -  -
 *
 * Note that for rows, InnoDB only acquires S or X locks.
 * For tables, InnoDB normally acquires IS or IX locks.
 * S or X table locks are only acquired for LOCK TABLES.
 * Auto-increment (AI) locks are needed because of
 * statement-level MySQL binlog.
 * See also lock_mode_compatible().
 */
static const byte lock_compatibility_matrix[5][5] = {
 /**         IS     IX       S     X       AI */
 /* IS */ {  TRUE,  TRUE,  TRUE,  FALSE,  TRUE},
 /* IX */ {  TRUE,  TRUE,  FALSE, FALSE,  TRUE},
 /* S  */ {  TRUE,  FALSE, TRUE,  FALSE,  FALSE},
 /* X  */ {  FALSE, FALSE, FALSE, FALSE,  FALSE},
 /* AI */ {  TRUE,  TRUE,  FALSE, FALSE,  FALSE}
};

/* STRONGER-OR-EQUAL RELATION (mode1=row, mode2=column)
 *    IS IX S  X  AI
 * IS +  -  -  -  -
 * IX +  +  -  -  -
 * S  +  -  +  -  -
 * X  +  +  +  +  +
 * AI -  -  -  -  +
 * See lock_mode_stronger_or_eq().
 */
static const byte lock_strength_matrix[5][5] = {
 /**         IS     IX       S     X       AI */
 /* IS */ {  TRUE,  FALSE, FALSE,  FALSE, FALSE},
 /* IX */ {  TRUE,  TRUE,  FALSE, FALSE,  FALSE},
 /* S  */ {  TRUE,  FALSE, TRUE,  FALSE,  FALSE},
 /* X  */ {  TRUE,  TRUE,  TRUE,  TRUE,   TRUE},
 /* AI */ {  FALSE, FALSE, FALSE, FALSE,  TRUE}
};

#define PRDT_HEAPNO	PAGE_HEAP_NO_INFIMUM
/** Record locking request status */
enum lock_rec_req_status {
        /** Failed to acquire a lock */
        LOCK_REC_FAIL,
        /** Succeeded in acquiring a lock (implicit or already acquired) */
        LOCK_REC_SUCCESS,
        /** Explicitly created a new lock */
        LOCK_REC_SUCCESS_CREATED
};

#ifdef UNIV_DEBUG
/** The count of the types of locks. */
static const ulint      lock_types = UT_ARR_SIZE(lock_compatibility_matrix);
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Gets the previous record lock set on a record.
@return previous lock on the same record, NULL if none exists */
const lock_t*
lock_rec_get_prev(
/*==============*/
	const lock_t*	in_lock,/*!< in: record lock */
	ulint		heap_no);/*!< in: heap number of the record */

/*********************************************************************//**
Checks if some transaction has an implicit x-lock on a record in a clustered
index.
@return transaction id of the transaction which has the x-lock, or 0 */
UNIV_INLINE
trx_id_t
lock_clust_rec_some_has_impl(
/*=========================*/
	const rec_t*		rec,	/*!< in: user record */
	const dict_index_t*	index,	/*!< in: clustered index */
	const rec_offs*		offsets)/*!< in: rec_get_offsets(rec, index) */
	MY_ATTRIBUTE((warn_unused_result));

/*********************************************************************//**
Gets the first or next record lock on a page.
@return next lock, NULL if none exists */
UNIV_INLINE
const lock_t*
lock_rec_get_next_on_page_const(
/*============================*/
	const lock_t*	lock);	/*!< in: a record lock */

/*********************************************************************//**
Gets the nth bit of a record lock.
@return TRUE if bit set also if i == ULINT_UNDEFINED return FALSE*/
UNIV_INLINE
ibool
lock_rec_get_nth_bit(
/*=================*/
	const lock_t*	lock,	/*!< in: record lock */
	ulint		i);	/*!< in: index of the bit */

/*********************************************************************//**
Gets the number of bits in a record lock bitmap.
@return number of bits */
UNIV_INLINE
ulint
lock_rec_get_n_bits(
/*================*/
	const lock_t*	lock);	/*!< in: record lock */

/**********************************************************************//**
Sets the nth bit of a record lock to TRUE. */
inline
void
lock_rec_set_nth_bit(
/*=================*/
	lock_t*	lock,	/*!< in: record lock */
	ulint	i);	/*!< in: index of the bit */

/** Reset the nth bit of a record lock.
@param[in,out] lock record lock
@param[in] i index of the bit that will be reset
@return previous value of the bit */
inline byte lock_rec_reset_nth_bit(lock_t* lock, ulint i)
{
	ut_ad(!lock->is_table());
#ifdef SUX_LOCK_GENERIC
	ut_ad(lock_sys.is_writer() || lock->trx->mutex_is_owner()
	      || lock_sys.is_cell_locked(*lock));
#else
	ut_ad(lock_sys.is_writer() || lock->trx->mutex_is_owner()
	      || lock_sys.is_cell_locked(*lock)
	      || (xtest() && !lock->trx->mutex_is_locked()));
#endif
	ut_ad(i < lock->un_member.rec_lock.n_bits);

	byte*	b = reinterpret_cast<byte*>(&lock[1]) + (i >> 3);
	byte	mask = byte(1U << (i & 7));
	byte	bit = *b & mask;
	*b &= byte(~mask);

	if (bit != 0) {
		ut_d(auto n=)
		lock->trx->lock.n_rec_locks--;
		ut_ad(n);
	}

	return(bit);
}

/** Gets the first or next record lock on a page.
@param lock a record lock
@return next lock, NULL if none exists */
UNIV_INLINE
lock_t *lock_rec_get_next_on_page(const lock_t *lock);

/*********************************************************************//**
Gets the next explicit lock request on a record.
@return next lock, NULL if none exists or if heap_no == ULINT_UNDEFINED */
UNIV_INLINE
lock_t*
lock_rec_get_next(
/*==============*/
	ulint	heap_no,/*!< in: heap number of the record */
	lock_t*	lock);	/*!< in: lock */

/*********************************************************************//**
Gets the next explicit lock request on a record.
@return next lock, NULL if none exists or if heap_no == ULINT_UNDEFINED */
UNIV_INLINE
const lock_t*
lock_rec_get_next_const(
/*====================*/
	ulint		heap_no,/*!< in: heap number of the record */
	const lock_t*	lock);	/*!< in: lock */

/** Get the first explicit lock request on a record.
@param cell     first lock hash table cell
@param id       page identifier
@param heap_no  record identifier in page
@return first lock
@retval nullptr if none exists */
inline lock_t *lock_sys_t::get_first(const hash_cell_t &cell, page_id_t id,
                                     ulint heap_no)
{
  lock_sys.assert_locked(cell);

  for (lock_t *lock= static_cast<lock_t*>(cell.node); lock; lock= lock->hash)
  {
    ut_ad(!lock->is_table());
    if (lock->un_member.rec_lock.page_id == id &&
        lock_rec_get_nth_bit(lock, heap_no))
      return lock;
  }
  return nullptr;
}

/*********************************************************************//**
Calculates if lock mode 1 is compatible with lock mode 2.
@return nonzero if mode1 compatible with mode2 */
UNIV_INLINE
ulint
lock_mode_compatible(
/*=================*/
	enum lock_mode	mode1,	/*!< in: lock mode */
	enum lock_mode	mode2);	/*!< in: lock mode */

/*********************************************************************//**
Calculates if lock mode 1 is stronger or equal to lock mode 2.
@return nonzero if mode1 stronger or equal to mode2 */
UNIV_INLINE
ulint
lock_mode_stronger_or_eq(
/*=====================*/
	enum lock_mode	mode1,	/*!< in: lock mode */
	enum lock_mode	mode2);	/*!< in: lock mode */

/*********************************************************************//**
Checks if a transaction has the specified table lock, or stronger. This
function should only be called by the thread that owns the transaction.
@return lock or NULL */
UNIV_INLINE
const lock_t*
lock_table_has(
/*===========*/
	const trx_t*		trx,	/*!< in: transaction */
	const dict_table_t*	table,	/*!< in: table */
	enum lock_mode		mode);	/*!< in: lock mode */

#include "lock0priv.inl"

#endif /* lock0priv_h */
