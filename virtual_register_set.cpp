#include "virtual_register_set.h"
#include "virtual_register.h"
#include "serial_exc.h"

#include <wbmqtt/utils.h>

#include <cassert>
#include <sstream>
#include <iostream>
#include <algorithm>

using namespace std;

TVirtualRegisterSet::TVirtualRegisterSet(const vector<PVirtualRegister> & virtualRegisters)
{
    VirtualRegisters.reserve(virtualRegisters.size());

    for (const auto & virtualRegister: virtualRegisters) {
        VirtualRegisters.push_back(virtualRegister);
    }
}

string TVirtualRegisterSet::GetTextValue() const
{
    ostringstream ss;
    bool first = true;

    for (const auto & virtualRegister: VirtualRegisters) {
        if (!first) {
            ss << ";";
        }

        ss << virtualRegister->GetTextValue();
        first = false;
    }

    return ss.str();
}

void TVirtualRegisterSet::SetTextValue(const string & value)
{
    const auto & textValues = StringSplit(value, ';');
    const auto valueCount = textValues.size();
    {
        const auto expectedValueCount = VirtualRegisters.size();

        if (expectedValueCount != valueCount) {
            throw TSerialDeviceException("value count mismatch for register set: expected '" +
                                            to_string(expectedValueCount) + "' actual '" +
                                            to_string(valueCount));
        }
    }

    for (size_t i = 0; i < valueCount; ++i) {
        const auto & virtualRegister = VirtualRegisters[i];

        if (true) {  // TODO: only in debug
            std::cerr << "setting device register: " << virtualRegister->ToString() << " <- " <<
                textValues[i] << std::endl;
        }

        virtualRegister->SetTextValue(textValues[i]);
    }
}

EErrorState TVirtualRegisterSet::GetErrorState() const
{
    EErrorState result = EErrorState::NoError;

    for (const auto & virtualRegister: VirtualRegisters) {
        Add(result, virtualRegister->GetErrorState());
    }

    return result;
}

bool TVirtualRegisterSet::ValueIsRead() const
{
    return all_of(VirtualRegisters.begin(), VirtualRegisters.end(), [](const PVirtualRegister & virtualRegister){
        return virtualRegister->ValueIsRead();
    });
}

bool TVirtualRegisterSet::IsChanged(EPublishData data) const
{
    return any_of(VirtualRegisters.begin(), VirtualRegisters.end(), [data](const PVirtualRegister & virtualRegister){
        return virtualRegister->IsChanged(data);
    });
}