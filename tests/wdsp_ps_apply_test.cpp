// PSApplyCorrection (WDSP patch 23) runs PureSignal's correction stage on IQ the
// caller supplies, for a host whose modulator is not WDSP's TXA chain. With no
// correction loaded the stage is not running, so the IQ passes through exactly.

#include "core/dsp/WdspChannel.h"

#include <aether_wdsp.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool value, const char* message)
{
    std::cout << (value ? "[ OK ] " : "[FAIL] ") << message << '\n';
    if (!value) {
        ++failures;
    }
}

std::vector<double> ramp(int samples)
{
    std::vector<double> iq(static_cast<std::size_t>(samples) * 2);
    for (std::size_t i = 0; i < iq.size(); ++i) {
        iq[i] = 0.5 * std::sin(0.01 * static_cast<double>(i));
    }
    return iq;
}

}   // namespace

int main()
{
    WdspChannel::Config tx;
    tx.direction = WdspChannel::Direction::Transmit;
    tx.inputSampleRate = 48000;
    tx.dspSampleRate = 48000;
    tx.outputSampleRate = 48000;
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(tx, &error);
    check(channel != nullptr, "a transmit channel opens");
    if (!channel) {
        std::cerr << error << '\n';
        return 1;
    }
    const int id = channel->channelId();

    check(PSCorrectionBusy(id) == 0, "no correction loaded: the stage is not busy");

    {
        const std::vector<double> in = ramp(1024);
        std::vector<double> out(in.size(), 9.0);
        PSApplyCorrection(id, 1024, const_cast<double*>(in.data()), out.data());
        check(out == in, "separate buffers: the IQ is copied unchanged");
    }
    {
        const std::vector<double> expected = ramp(333);
        std::vector<double> inPlace = expected;
        PSApplyCorrection(id, 333, inPlace.data(), inPlace.data());
        check(inPlace == expected, "in place, another size: the IQ is left unchanged");
    }
    {
        // A block larger than the channel's own would overrun if the size were
        // ignored; a block smaller must touch nothing past its end.
        const std::vector<double> in = ramp(16);
        std::vector<double> out(64, 7.0);
        PSApplyCorrection(id, 16, const_cast<double*>(in.data()), out.data());
        check(out[31] == in[31] && out[32] == 7.0, "only the requested samples are written");
    }

    channel.reset();
    std::cout << (failures == 0 ? "all checks passed\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
