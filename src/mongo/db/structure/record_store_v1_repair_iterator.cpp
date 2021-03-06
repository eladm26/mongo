/**
 *    Copyright (C) 2014 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/structure/record_store_v1_repair_iterator.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/structure/record_store_v1_simple.h"

namespace mongo {

    RecordStoreV1RepairIterator::RecordStoreV1RepairIterator(const RecordStoreV1Base* recordStore)
        : _recordStore(recordStore), _stage(FORWARD_SCAN) {
        
        // Position the iterator at the first record
        //
        getNext();
    }

    bool RecordStoreV1RepairIterator::isEOF() {
        return _currRecord.isNull();
    }

    DiskLoc RecordStoreV1RepairIterator::curr() { return _currRecord; }

    DiskLoc RecordStoreV1RepairIterator::getNext() {
        DiskLoc retVal = _currRecord;

        const ExtentManager* em = _recordStore->_extentManager;

        while (true) {
            if (_currRecord.isNull()) {

                if (!_advanceToNextValidExtent()) {
                    return retVal;
                }

                _seenInCurrentExtent.clear();

                // Otherwise _advanceToNextValidExtent would have returned false
                //
                invariant(!_currExtent.isNull());

                const Extent* e = em->getExtent(_currExtent, false);
                _currRecord = (FORWARD_SCAN == _stage ? e->firstRecord : e->lastRecord);
            }
            else {
                switch (_stage) {
                case FORWARD_SCAN:
                    _currRecord = _recordStore->getNextRecordInExtent(_currRecord);
                    break;
                case BACKWARD_SCAN:
                    _currRecord = _recordStore->getPrevRecordInExtent(_currRecord);
                    break;
                default:
                    invariant(!"This should never be reached.");
                    break;
                }
            }

            if (_currRecord.isNull()) {
                continue;
            }

            // Validate the contents of the record's disk location and deduplicate
            //
            if (!_seenInCurrentExtent.insert(_currRecord).second) {
                error() << "infinite loop in extent, seen: " << _currRecord << " before" << endl;
                _currRecord = DiskLoc();
                continue;
            }

            if (_currRecord.getOfs() <= 0){
                error() << "offset is 0 for record which should be impossible" << endl;
                _currRecord = DiskLoc();
                continue;
            }

            return retVal;
        }
    }

    bool RecordStoreV1RepairIterator::_advanceToNextValidExtent() {
        const ExtentManager* em = _recordStore->_extentManager;

        while (true) {
            if (_currExtent.isNull()) {
                switch (_stage) {
                case FORWARD_SCAN:
                    _currExtent = _recordStore->details()->firstExtent();
                    break;
                case BACKWARD_SCAN:
                    _currExtent = _recordStore->details()->lastExtent();
                    break;
                default:
                    invariant(DONE == _stage);
                    return false;
                }
            }
            else {
                // If _currExtent is not NULL, then it must point to a valid extent, so no extra
                // checks here.
                //
                const Extent* e = em->getExtent(_currExtent, false);
                _currExtent = (FORWARD_SCAN == _stage ? e->xnext : e->xprev);
            }

            bool hasNextExtent = !_currExtent.isNull();

            // Sanity checks for the extent's disk location
            //
            if (hasNextExtent && (!_currExtent.isValid() || (_currExtent.getOfs() <= 0))) {
                error() << "Invalid extent location: " << _currExtent << endl;

                // Switch the direction of scan
                //
                hasNextExtent = false;
            }

            if (hasNextExtent) {
                break;
            }

            // Swap the direction of scan and loop again
            //
            switch (_stage) {
            case FORWARD_SCAN:
                _stage = BACKWARD_SCAN;
                break;
            case BACKWARD_SCAN:
                _stage = DONE;
                break;
            default:
                invariant(!"This should never be reached.");
                break;
            }

            _currExtent = DiskLoc();
        }


        // Check _currExtent's contents for validity, but do not count is as failure if they
        // don't check out.
        //
        const Extent* e = em->getExtent(_currExtent, false);
        if (!e->isOk()){
            warning() << "Extent not ok magic: " << e->magic << " going to try to continue"
                << endl;
        }

        log() << (FORWARD_SCAN == _stage ? "FORWARD" : "BACKWARD") << "  Extent loc: " 
              << _currExtent << ", length: " << e->length << endl;

        return true;
    }

    void RecordStoreV1RepairIterator::invalidate(const DiskLoc& dl) {
        verify(!"Invalidate is not supported for RecordStoreV1RepairIterator.");
    }

    const Record* RecordStoreV1RepairIterator::recordFor(const DiskLoc& loc) const {
        return _recordStore->recordFor( loc );
    }

}  // namespace mongo
