#include "ir_device_query_factory.h"
#include "ir_device_query.h"
#include "serial_device.h"

#include <iostream>
#include <cassert>

using namespace std;

namespace // utility
{
    uint32_t GetMaxHoleSize(const TPSet<PProtocolRegister> & registerSet)
    {
        uint32_t hole = 0;

        int prev = -1;
        for (const auto & reg: registerSet) {
            if (prev >= 0) {
                hole = max(hole, reg->Address - prev);
            }

            prev = reg->Address;
        }

        return hole;
    }

    bool IsReadOperation(EQueryOperation operation)
    {
        switch(operation) {
            case EQueryOperation::Read:
                return true;
            case EQueryOperation::Write:
                return false;
            default:
                assert(false);
                throw TSerialDeviceException("unknown operation: " + to_string((int)operation));
        }
    }

    template <class Query>
    void AddQueryImpl(const TPSet<PProtocolRegister> & registerSet, TPSet<PIRDeviceQuery> & result)
    {
        bool inserted = result.insert(TIRDeviceQueryFactory::CreateQuery<Query>(registerSet)).second;
        assert(inserted);
    }

    template <class Query>
    void AddQueryImpl(const TPSet<PProtocolRegister> & registerSet, list<PIRDeviceQuery> & result)
    {
        result.push_back(TIRDeviceQueryFactory::CreateQuery<Query>(registerSet));
    }

    template <class Query>
    void AddQuery(const TPSet<PProtocolRegister> & registerSet, TQueries & result)
    {
        AddQueryImpl<Query>(registerSet, result);
    }
}

TQueries TIRDeviceQueryFactory::GenerateQueries(list<TPSet<PProtocolRegister>> && registerSets, bool enableHoles, EQueryOperation operation)
{
    assert(!registerSets.empty());
    assert(!registerSets.front().empty());

    /** gathering data **/
    const auto & device = (*registerSets.front().begin())->GetDevice();

    const auto & deviceConfig = device->DeviceConfig();
    const auto & protocolInfo = device->GetProtocolInfo();

    const bool singleBitType = protocolInfo.IsSingleBitType((*registerSets.front().begin())->Type);
    const bool isRead = IsReadOperation(operation);

    const auto & addQuery = isRead ? AddQuery<TIRDeviceQuery>
                                   : singleBitType ? AddQuery<TIRDeviceSingleBitQuery>
                                                   : AddQuery<TIRDevice64BitQuery>;

    const int maxHole = enableHoles ? (singleBitType ? deviceConfig->MaxBitHole
                                                     : deviceConfig->MaxRegHole)
                                    : 0;
    int maxRegs;

    if (isRead) {
        if (singleBitType) {
            maxRegs = protocolInfo.GetMaxReadBits();
        } else {
            if ((deviceConfig->MaxReadRegisters > 0) && (deviceConfig->MaxReadRegisters <= protocolInfo.GetMaxReadRegisters())) {
                maxRegs = deviceConfig->MaxReadRegisters;
            } else {
                maxRegs = protocolInfo.GetMaxReadRegisters();
            }
        }
    } else {
        maxRegs = singleBitType ? protocolInfo.GetMaxWriteBits() : protocolInfo.GetMaxWriteRegisters();
    }
    /** done gathering data **/

    cerr << "merging sets" << endl;
    MergeSets(registerSets, static_cast<uint32_t>(maxHole), static_cast<uint32_t>(maxRegs));
    cerr << "merging sets done" << endl;

    TQueries result;

    for (auto & registerSet: registerSets) {
        addQuery(move(registerSet), result);
    }

    assert(!result.empty());

    return result;
}

/**
 * Following algorihm:
 *  1) tries to reduce number of sets in passed list
 *  2) ensures that maxHole and maxRegs are not exceeded
 *  3) allows same register to appear in different sets if those sets couldn't merge (same register will be read more than once during same cycle)
 *  4) doesn't split initial sets (registers that were in one set will stay in one set)
 */
void TIRDeviceQueryFactory::MergeSets(list<TPSet<PProtocolRegister>> & registerSets, uint32_t maxHole, uint32_t maxRegs)
{
    enum EStage: uint8_t {
        HoleEliminate,
        Merge,
        Done
    };

    uint8_t Stage = HoleEliminate;

    while (Stage != Done) {
        cerr << "begin " << (int)Stage << " stage" << endl;
        for (auto itRegisterSet = registerSets.begin(); itRegisterSet != registerSets.end(); ++itRegisterSet) {
            auto & registerSet = *itRegisterSet;
            auto holeSize = GetMaxHoleSize(registerSet);

            bool done = false;
            while (!done) {
                map<uint32_t, map<uint32_t, vector<list<TPSet<PProtocolRegister>>::iterator>>> setsForMerge;

                for (auto itOtherRegisterSet = registerSets.begin(); itOtherRegisterSet != registerSets.end(); ++itOtherRegisterSet) {
                    const auto & otherRegisterSet = *itOtherRegisterSet;

                    if (itRegisterSet == itOtherRegisterSet) {
                        continue;
                    }

                    uint32_t start     = (*registerSet.begin())->Address,
                            end        = (*registerSet.rbegin())->Address + 1,
                            startOther = (*otherRegisterSet.begin())->Address,
                            endOther   = (*otherRegisterSet.rbegin())->Address + 1;

                    bool overlap = startOther < end && start < endOther;

                    if (overlap || Stage == Merge) {

                        if (!overlap) {
                            uint32_t distance = max(start, startOther) - min(end, endOther);

                            if (distance > maxHole) {
                                continue;
                            }
                        }

                        // size after merge
                        uint32_t size = end - start;
                        uint32_t otherSize = endOther - startOther;
                        uint32_t minMergedSize = max(size, otherSize);
                        uint32_t mergedSize = max(end, endOther) - min(start, startOther);

                        if (size > maxRegs) {
                            throw TSerialDeviceException("unable to create queries for given register configuration: max reg count exceeded");
                        }

                        if (mergedSize > maxRegs) {
                            continue;
                        }

                        // EXPL: difference between actual size after merge and minimal possible size after merge
                        //       shows how much sets overlap each other as intervals (0 - means one set covers another entirely)
                        uint32_t sortingFactor = mergedSize - minMergedSize;

                        setsForMerge[sortingFactor][otherSize].push_back(itOtherRegisterSet);
                    }
                }

                bool holeSizeDecreased = false;
                bool merged = false;
                for (const auto & sortFactorRegisterSetsBySize: setsForMerge) {
                    const auto & registerSetsBySize = sortFactorRegisterSetsBySize.second;

                    for (const auto & sizeRegisterSets: registerSetsBySize) {
                        const auto & setsForMerge = sizeRegisterSets.second;

                        for (const auto & setForMerge: setsForMerge) {
                            if (Stage == HoleEliminate) {
                                if (holeSize > maxHole) {
                                    auto mergedSet = registerSet;
                                    mergedSet.insert(setForMerge->begin(), setForMerge->end());
                                    auto newHoleSize = GetMaxHoleSize(mergedSet);
                                    holeSizeDecreased = newHoleSize < holeSize;

                                    if (holeSizeDecreased) {
                                        registerSets.erase(setForMerge);
                                        registerSet = move(mergedSet);
                                        holeSize = newHoleSize;
                                        merged = true;
                                        break;
                                    }
                                }
                            } else {
                                registerSet.insert(setForMerge->begin(), setForMerge->end());
                                registerSets.erase(setForMerge);
                                merged = true;
                                break;
                            }
                        }

                        if (merged)
                            break;
                    }

                    if (merged)
                        break;
                }

                if (Stage == HoleEliminate) {
                    if (holeSize <= maxHole) {
                        done = true;
                    } else if (!holeSizeDecreased) {
                        throw TSerialDeviceException("unable to create queries for given register configuration: max hole count exceeded");
                    }
                } else if (Stage == Merge) {
                    if (!merged) {
                        done = true;
                    }
                }
            }
        }

        cerr << "end " << (int)Stage << " stage" << endl;
        ++Stage;
    }
}