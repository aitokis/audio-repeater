#include "audio_core.h"
#include "allocator_hooks.h"
#include <samplerate.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
std::uint64_t checks = 0;
void require(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(x) require((x), #x, __LINE__)
constexpr std::uint32_t seed = 0x5A17C0DE;
constexpr double pi = std::numbers::pi;
using audio::Encoding;
using audio::Format;

template<class F> void rejects(F f) {
    bool threw = false;
    try { f(); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
void formats() {
    // The requested 48 kHz PCM24 -> PCM16 conversion, including signed halfway
    // cases, saturation and ignored low padding bits in valid24-in-32 capture.
    constexpr std::array<std::int32_t,14> pcm24{-8388608,-8388607,-129,-128,-127,-1,0,1,127,128,129,8388479,8388480,8388607};
    constexpr std::array<std::int32_t,14> pcm16{-32768,-32768,-1,-1,0,0,0,0,0,1,1,32767,32767,32767};
    for (const auto encoding : {Encoding::pcm24, Encoding::pcm32}) {
        Format capture{encoding,48000,1,24,encoding == Encoding::pcm24 ? 3U : 4U,4};
        Format render{Encoding::pcm16,48000,1,16,2,4};
        std::vector<std::byte> input_bytes(pcm24.size() * capture.block_align);
        std::array<std::byte,pcm24.size() * 2> output_bytes{};
        std::array<float,pcm24.size()> floats{};
        for (std::size_t i = 0; i < pcm24.size(); ++i) {
            auto raw = static_cast<std::uint32_t>(pcm24[i]);
            if (encoding == Encoding::pcm32) raw = (raw << 8) | 0xA5;
            for (unsigned b = 0; b < capture.block_align; ++b)
                input_bytes[i * capture.block_align + b] = static_cast<std::byte>((raw >> (8*b)) & 255);
        }
        CHECK(audio::decode(capture,input_bytes,floats,1));
        CHECK(audio::encode(render,floats,1,output_bytes));
        for (std::size_t i = 0; i < pcm16.size(); ++i) {
            const auto raw = std::to_integer<unsigned>(output_bytes[2*i]) |
                (std::to_integer<unsigned>(output_bytes[2*i+1]) << 8);
            CHECK(raw == (static_cast<std::uint32_t>(pcm16[i]) & 65535));
        }
    }
    const std::array encodings{Encoding::float32, Encoding::pcm16, Encoding::pcm24, Encoding::pcm32, Encoding::pcm32};
    const std::array bits{32U,16U,24U,24U,32U};
    const std::array<float, 11> input{-2,-1,-0.5F,0,0.5F,1,2,
        std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::max()};
    for (std::size_t k = 0; k < encodings.size(); ++k) {
        Format f{encodings[k],48000,1,bits[k],0,4};
        f.block_align = f.bytes_per_sample();
        CHECK(f.valid());
        std::vector<std::byte> bytes(input.size() * f.block_align);
        std::vector<float> decoded(input.size());
        CHECK(audio::encode(f,input,1,bytes));
        CHECK(audio::decode(f,bytes,decoded,1));
        for (std::size_t i = 0; i < input.size(); ++i) {
            CHECK(std::isfinite(decoded[i]));
            const double expected = !std::isfinite(input[i]) ? 0 :
                f.encoding == Encoding::float32 ? input[i] : std::clamp(input[i],-1.0F,1.0F);
            CHECK(std::abs(decoded[i] - expected) <= (f.valid_bits == 16 ? 1.0/32768 : f.valid_bits == 24 ? 1.0/8388608 : 1e-7));
        }
        const std::uint32_t minimum = f.encoding == Encoding::float32 ? 0xBF800000U :
            f.encoding == Encoding::pcm16 ? 0x8000U : f.encoding == Encoding::pcm24 ? 0x800000U : 0x80000000U;
        const std::uint32_t maximum = f.encoding == Encoding::float32 ? 0x3F800000U :
            f.encoding == Encoding::pcm16 ? 0x7FFFU : f.encoding == Encoding::pcm24 ? 0x7FFFFFU :
            f.valid_bits == 24 ? 0x7FFFFF00U : 0x7FFFFFFFU;
        for(unsigned b=0;b<f.bytes_per_sample();++b) {
            CHECK(std::to_integer<unsigned>(bytes[f.block_align+b]) == ((minimum>>(8*b))&255));
            CHECK(std::to_integer<unsigned>(bytes[5*f.block_align+b]) == ((maximum>>(8*b))&255));
        }
        CHECK(audio::decode(f,{},decoded,1,true));
        CHECK(std::all_of(decoded.begin(),decoded.end(),[](float x){return x==0;}));
        auto bad = f; bad.valid_bits = 17; CHECK(!bad.valid());
        bad = f; bad.block_align++; CHECK(!bad.valid());
        bad = f; bad.channel_mask = 8; CHECK(!bad.valid());
        bad = f; bad.rate = 7999; CHECK(!bad.valid());
        bad = f; bad.rate = 192001; CHECK(!bad.valid());
        bad = f; bad.channels = 3; CHECK(!bad.valid());
        CHECK(!audio::decode(bad,bytes,decoded,1));
        CHECK(!audio::encode(f,input,3,bytes));
        CHECK(!audio::decode(f,std::span(bytes).first(bytes.size()-1),decoded,1));
    }
    Format mono{Encoding::float32,48000,1,32,4,4};
    Format stereo{Encoding::float32,48000,2,32,8,3};
    std::array<float,2> pair{0.75F,-0.25F}, result{};
    std::array<std::byte,8> bytes{};
    CHECK(audio::encode(stereo,pair,2,bytes));
    std::array<float,1> average{};
    CHECK(audio::decode(stereo,bytes,average,1)); CHECK(average[0] == 0.25F);
    CHECK(audio::encode(mono,pair,2,std::span(bytes).first(4)));
    CHECK(audio::decode(mono,std::span(bytes).first(4),result,2));
    CHECK(result[0] == 0.25F && result[1] == 0.25F);
    CHECK(audio::encode(stereo,average,1,bytes));
    CHECK(audio::decode(stereo,bytes,result,2));
    CHECK(result[0] == 0.25F && result[1] == 0.25F);
    std::array<std::byte,4> msb{std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
    Format pcm{Encoding::pcm32,48000,1,24,4,0};
    CHECK(audio::decode(pcm,msb,average,1)); CHECK(average[0] == -1.0F/8388608);
    std::array<float,1> tiny{std::numeric_limits<float>::denorm_min()}, tiny_out{};
    CHECK(audio::encode(mono,tiny,1,std::span(bytes).first(4)));
    CHECK(audio::decode(mono,std::span(bytes).first(4),tiny_out,1)); CHECK(tiny_out==tiny);
    mono.encoding = static_cast<Encoding>(99); CHECK(!mono.valid());
}
void fifo() {
    rejects([]{audio::Fifo q(0,1);});
    rejects([]{audio::Fifo q(3,3);});
    audio::Fifo q(7,2);
    std::array<float,20> input{}, output{};
    for (std::size_t i=0;i<input.size();++i) input[i]=static_cast<float>(i);
    CHECK(q.peek(output)==0);
    CHECK(q.push(input)==7); CHECK(q.size()==7);
    CHECK(q.push(input)==0);
    q.discard(5); CHECK(q.size()==2);
    CHECK(q.push(std::span(input).first(10))==5);
    CHECK(q.peek(std::span(output).first(14))==7);
    for(std::size_t i=0;i<4;++i) CHECK(output[i]==input[i+10]);
    for(std::size_t i=4;i<14;++i) CHECK(output[i]==input[i-4]);
    q.discard(100); CHECK(q.size()==0);
    CHECK(q.push(std::span(input).first(3))==0);
    std::mt19937 rng(seed);
    audio::Fifo random(113,1);
    std::uint64_t produced=0, consumed=0;
    for(int n=0;n<20000;++n) {
        const std::size_t count=1+rng()%20;
        for(std::size_t i=0;i<count;++i) input[i]=static_cast<float>(produced+i);
        const auto written=random.push(std::span(input).first(count)); produced+=written;
        const auto read=random.peek(std::span(output).first(1+rng()%20));
        for(std::size_t i=0;i<read;++i) CHECK(output[i]==static_cast<float>(consumed+i));
        consumed+=read; random.discard(read);
        CHECK(random.size()==produced-consumed); CHECK(random.size()<=random.capacity());
    }
    random.clear(); CHECK(random.size()==0);
    input[0]=std::numeric_limits<float>::quiet_NaN();
    CHECK(random.push(std::span(input).first(1))==1);
    CHECK(random.peek(std::span(output).first(1))==1); CHECK(output[0]==0);
    std::printf("FIFO random iterations=20000 produced=%llu consumed=%llu\n",
        static_cast<unsigned long long>(produced), static_cast<unsigned long long>(consumed));
}
// Persistent live conversion; trailing zero input supplies filter lookahead.
// No end_of_input flushing, matching the streaming contract.
std::vector<float> convert(unsigned in_rate, unsigned out_rate, std::span<const float> input,
                           unsigned channels, std::size_t in_chunk, std::size_t out_chunk,
                           double correction=0) {
    audio::Resampler src(in_rate,out_rate,channels);
    const double ratio=src.nominal_ratio()/(1+correction*1e-6);
    std::vector<float> result;
    std::vector<float> output(out_chunk*channels);
    std::size_t offset=0;
    for(std::size_t attempt=0;attempt<input.size()+10000;++attempt) {
        const auto count=std::min(in_chunk*channels,input.size()-offset);
        const auto r=src.process(input.subspan(offset,count),output,ratio);
        CHECK(r.error==0); CHECK(r.consumed<=count/channels); CHECK(r.generated<=out_chunk);
        offset+=r.consumed*channels;
        result.insert(result.end(),output.begin(),output.begin()+static_cast<std::ptrdiff_t>(r.generated*channels));
        CHECK(src.pending_input()>=-1e-5);
        CHECK(src.pending_input()<=static_cast<double>(in_chunk+src.lookahead()+2));
        if(!r.consumed && !r.generated) { CHECK(offset==input.size()); break; }
        CHECK(attempt+1<input.size()+10000);
    }
    for(float x:result) CHECK(std::isfinite(x));
    return result;
}
void src_tests() {
    for(const auto& [in_rate,out_rate]: std::array<std::pair<unsigned,unsigned>,5>{{
            {48000,48000},{44100,48000},{48000,44100},{8000,192000},{192000,8000}}}) {
        for(double ppm:std::array{-2000.0,0.0,2000.0}) {
            std::vector<float> input(in_rate/4+5000,0);
            const auto impulse=in_rate/20;
            input[impulse]=0.5F;
            const auto a=convert(in_rate,out_rate,input,1,127,193,ppm);
            const auto b=convert(in_rate,out_rate,input,1,1021,61,ppm);
            CHECK(a.size()==b.size());
            double difference=0;
            for(std::size_t i=0;i<a.size();++i) difference=std::max(difference,std::abs(double(a[i]-b[i])));
            CHECK(difference<2e-6);
            const auto peak=static_cast<std::size_t>(std::max_element(a.begin(),a.end())-a.begin());
            const double ratio=double(out_rate)/in_rate/(1+ppm*1e-6);
            CHECK(std::abs(double(peak)-impulse*ratio)<2);
            CHECK(std::abs(double(a.size())-static_cast<double>(input.size())*ratio)<2000*std::max(1.0,ratio));
            std::printf("SRC %u->%u ppm=%g frames=%zu impulse_peak=%zu chunk_max_error=%.9g\n",
                in_rate,out_rate,ppm,a.size(),peak,difference);
        }
    }
    // Measure the amount of real input needed before the first impulse output.
    audio::Resampler src(48000,48000,1);
    std::array<float,1> input{1}, output{};
    std::size_t delay=0;
    for(;delay<200;++delay) {
        const auto r=src.process(input,output,1); input[0]=0;
        CHECK(r.error==0); CHECK(r.consumed==1);
        if(r.generated) break;
    }
    CHECK(delay>0 && delay<src.lookahead());
    std::printf("SRC startup filter lookahead=%zu input frames; budget=%zu\n",delay,src.lookahead());
    CHECK(src.process(input,output,1.01).error!=0);
    CHECK(src.process(input,output,std::numeric_limits<double>::quiet_NaN()).error!=0);
    src.reset(); CHECK(src.pending_input()==0);
    rejects([]{audio::Resampler s(0,48000,1);});
    CHECK(src.process(input,{},1).consumed==0);
    // Demand-fed transport at both rate extremes, including the clock bounds.
    // Five seconds each exercises physical FIFO and hidden SRC queue together.
    for(const auto& [in_rate,out_rate]:std::array<std::pair<unsigned,unsigned>,2>{{{8000,192000},{192000,8000}}}) {
        for(double ppm:std::array{-2000.0,2000.0}) {
            const std::size_t target=in_rate/20;
            const std::size_t block=out_rate/100;
            audio::Transport core(in_rate,out_rate,1,target*4,target,block);
            std::vector<float> capture(in_rate/100+10,0.1F), transport_output(block);
            double fraction=0;
            for(int cycle=0;cycle<500;++cycle) {
                fraction+=double(in_rate)/100*(1+ppm*1e-6);
                const auto count=static_cast<std::size_t>(fraction);
                fraction-=static_cast<double>(count);
                CHECK(core.push(std::span(capture).first(count)));
                core.render(transport_output);
                CHECK(core.queued_input()<=static_cast<double>(core.capacity()));
                CHECK(core.internal_input()<=double(in_rate)/100+2000);
                for(float x:transport_output) CHECK(std::isfinite(x));
            }
            CHECK(core.statistics().underflows==0); CHECK(core.statistics().overflows==0);
            CHECK(core.statistics().resyncs==0); CHECK(core.statistics().src_errors==0);
            CHECK(core.statistics().generated_frames+core.statistics().startup_silent_frames==core.statistics().output_frames);
            std::printf("TRANSPORT %u->%u ppm=%g cycles=500 capacity=%zu queue=%.3f internal=%.3f consumed=%llu generated=%llu\n",
                in_rate,out_rate,ppm,core.capacity(),core.queued_input(),core.internal_input(),
                static_cast<unsigned long long>(core.statistics().consumed_frames),
                static_cast<unsigned long long>(core.statistics().generated_frames));
        }
    }
    // Stereo channel isolation, repeated streaming calls and reset.
    std::vector<float> stereo(8192*2,0);
    stereo[400*2]=0.5F;
    const auto a=convert(44100,48000,stereo,2,71,137);
    for(std::size_t i=1;i<a.size();i+=2) CHECK(a[i]==0);
}
double tone_amplitude(std::span<const float> samples, double frequency, unsigned rate) {
    // Fixed 1-second rectangular coherent window, starting 1/2 second in.
    const std::size_t start=rate/2, count=rate;
    CHECK(samples.size()>=start+count);
    double sine=0, cosine=0;
    for(std::size_t i=0;i<count;++i) {
        const double phase=2*pi*frequency*double(start+i)/rate;
        sine+=samples[start+i]*std::sin(phase);
        cosine+=samples[start+i]*std::cos(phase);
    }
    return 2*std::hypot(sine,cosine)/static_cast<double>(count);
}
void quality() {
    for(const auto& [in_rate,out_rate]:std::array<std::pair<unsigned,unsigned>,2>{{{44100,48000},{48000,44100}}}) {
        for(double frequency:std::array{1000.0,5000.0,10000.0}) {
            std::vector<float> input(in_rate*2);
            for(std::size_t i=0;i<input.size();++i) input[i]=static_cast<float>(0.25*std::sin(2*pi*frequency*static_cast<double>(i)/in_rate));
            const auto output=convert(in_rate,out_rate,input,1,317,509);
            const double gain=20*std::log10(tone_amplitude(output,frequency,out_rate)/0.25);
            std::printf("QUALITY %u->%u tone=%gHz window=[0.5,1.5)s rectangular gain=%+.6fdB\n",in_rate,out_rate,frequency,gain);
            CHECK(std::abs(gain)<=0.2);
        }
    }
    std::vector<float> input(96000*2);
    for(std::size_t i=0;i<input.size();++i) input[i]=static_cast<float>(0.25*std::sin(2*pi*30000*static_cast<double>(i)/96000));
    const auto output=convert(96000,48000,input,1,317,509);
    const double alias=20*std::log10(tone_amplitude(output,18000,48000)/0.25);
    std::printf("QUALITY 96000->48000 30000Hz alias18000Hz=%fdB window=[0.5,1.5)s rectangular\n",alias);
    CHECK(alias<=-60);
}

void simulate(double ppm, unsigned seconds) {
    constexpr unsigned rate=8000, block=80;
    audio::Transport core(rate,rate,1,2048,400,block);
    std::array<float,100> capture{};
    std::array<float,block> output{};
    std::mt19937 rng(seed);
    for(std::size_t i=0;i<capture.size();++i) capture[i]=static_cast<float>((int(rng()%1000)-500)/2000.0);
    double fraction=0, min_queue=1e9, max_queue=0, start_queue=0, end_queue=0;
    double late_first=0, late_second=0;
    audio::Statistics warm{};
    for(unsigned step=0;step<seconds*100;++step) {
        fraction+=block*(1+ppm*1e-6);
        auto frames=static_cast<std::size_t>(std::floor(fraction)); fraction-=static_cast<double>(frames);
        while(frames) {
            const auto chunk=std::min(frames,std::size_t(1+rng()%37));
            CHECK(core.push(std::span(capture).first(chunk))); frames-=chunk;
        }
        core.render(output);
        CHECK(core.queued_input()<=static_cast<double>(core.capacity())+1e-6);
        CHECK(core.internal_input()>=-1e-4);
        CHECK(core.internal_input()<240);
        CHECK(std::abs(core.correction_ppm())<=2000.00001);
        if(step==2999) { warm=core.statistics(); start_queue=core.queued_input(); }
        if(step>=3000) {
            min_queue=std::min(min_queue,core.queued_input()); max_queue=std::max(max_queue,core.queued_input());
            for(float x:output) CHECK(std::isfinite(x));
        }
        end_queue=core.queued_input();
        if(step>=seconds*100-6000 && step<seconds*100-3000) late_first+=end_queue/3000;
        if(step>=seconds*100-3000) late_second+=end_queue/3000;
    }
    const auto& stats=core.statistics();
    std::printf("DRIFT ppm=%+.0f virtual_seconds=%u warmup=30 queue=[%.3f,%.3f] first=%.3f last=%.3f correction=%.6f under=%llu over=%llu resync=%llu consumed=%llu generated=%llu\n",
        ppm,seconds,min_queue,max_queue,start_queue,end_queue,core.correction_ppm(),
        static_cast<unsigned long long>(stats.underflows-warm.underflows),
        static_cast<unsigned long long>(stats.overflows-warm.overflows),
        static_cast<unsigned long long>(stats.resyncs-warm.resyncs),
        static_cast<unsigned long long>(stats.consumed_frames),static_cast<unsigned long long>(stats.generated_frames));
    CHECK(stats.underflows==warm.underflows); CHECK(stats.overflows==warm.overflows); CHECK(stats.resyncs==warm.resyncs);
    CHECK(stats.src_errors==0); CHECK(max_queue-min_queue<200);
    CHECK(std::abs(end_queue-start_queue)<200);
    std::printf("DRIFT late_30s_means first=%.6f last=%.6f difference=%+.6f frames\n",late_first,late_second,late_second-late_first);
    CHECK(std::abs(late_second-late_first)<0.1);
    CHECK(stats.generated_frames+stats.startup_silent_frames+stats.silent_frames==stats.output_frames);
}
void capacity_test() {
    audio::Transport core(8000,8000,1,2048,400,80);
    std::array<float,400> input{};
    std::array<float,80> output{};
    CHECK(core.push(input)); CHECK(core.render(output)==80);
    CHECK(core.internal_input()>1);
    std::array<float,2048> full{};
    const bool accepted=core.push(full);
    CHECK(core.queued_input()<=static_cast<double>(core.capacity()));
    CHECK(!accepted);
    CHECK(core.statistics().overflows==1); CHECK(core.statistics().resyncs==1);
    core.discontinuity();
    CHECK(core.queued_input()==0); CHECK(core.correction_ppm()==0);
    CHECK(core.statistics().discontinuities==1);
    CHECK(core.render(output)==0);
    input.fill(0.5F);
    CHECK(core.push(input)); CHECK(core.render(output)==80);
    core.discontinuity();
    input.fill(0);
    CHECK(core.push(input)); CHECK(core.render(output)==80);
    for(float x:output) CHECK(x==0); // No pre-discontinuity SRC history survives.
}
void drift() {
    audio::DriftController c;
    CHECK(c.update(0.06,0.05,0.01)==0);
    for(int i=0;i<200;++i) c.update(0.06,0.05,0.1);
    CHECK(c.ppm()>0); CHECK(c.ppm()<=2000);
    c.reset(); CHECK(c.ppm()==0);
    double last=0;
    for(int i=0;i<200;++i) {
        c.update(1,0.05,0.1); CHECK(std::abs(c.ppm()-last)<=20.00001); last=c.ppm();
    }
    for(int i=0;i<500;++i) c.update(0,0.05,0.1);
    CHECK(c.ppm()<0); // Antiwindup permits leaving saturation.
    for(double ppm:std::array{-2000.0,-500.0,-100.0,100.0,500.0,2000.0}) simulate(ppm,1800);
    capacity_test();
    rejects([]{audio::Transport c(8000,8000,1,100,50,80);});
    // Beyond correction range, repeated 10ms capture/render produces explicit recovery.
    for(double skew:std::array{-0.05,0.05}) {
        audio::Transport core(8000,8000,1,2048,400,80);
        std::array<float,84> input{}; std::array<float,80> output{};
        for(int i=0;i<20000;++i) {
            core.push(std::span(input).first(skew<0?76:84)); core.render(output);
        }
        CHECK(core.statistics().resyncs>0);
        CHECK(skew<0 ? core.statistics().underflows>0 : core.statistics().overflows>0);
        std::printf("RECOVERY skew=%g under=%llu over=%llu resync=%llu\n",skew,
            static_cast<unsigned long long>(core.statistics().underflows),
            static_cast<unsigned long long>(core.statistics().overflows),
            static_cast<unsigned long long>(core.statistics().resyncs));
    }
}
void allocation() {
    // First prove both instrumentation routes detect allocations.
    audio_test_track(1);
    void* p=audio_test_malloc(8); audio_test_free(p);
    auto* q=::operator new(8); ::operator delete(q);
    CHECK(audio_test_allocations()==2); audio_test_track(0);
    audio::Transport core(8000,192000,2,8192,1600,1920);
    std::array<float,160> input{}; std::array<float,3840> output{};
    for(int i=0;i<100;++i) { core.push(input); core.render(output); }
    audio::Resampler down(192000,8000,2);
    std::array<float,3840> high{}; std::array<float,160> low{};
    down.process(high,low,down.nominal_ratio());
    Format format{Encoding::pcm24,48000,2,24,6,3};
    std::array<std::byte,480> packed{};
    std::array<float,160> decoded{};
    audio_test_track(1);
    for(int i=0;i<4000;++i) {
        core.push(input); core.render(output);
        CHECK(audio::encode(format,input,2,packed));
        CHECK(audio::decode(format,packed,decoded,2));
        const double ppm=2000*std::sin(i*0.01);
        const auto r=down.process(high,low,down.nominal_ratio()/(1+ppm*1e-6));
        CHECK(r.error==0);
        if(i%700==0) { core.discontinuity(); down.reset(); }
    }
    const auto count=audio_test_allocations(); audio_test_track(0);
    std::printf("ALLOCATIONS after_warmup=100 cycles=4000 channels=2 varying_ratio_and_resets count=%zu\n",count);
    CHECK(count==0);
    CHECK(core.statistics().src_errors==0);
}
}
int main(int argc,char** argv) {
    try {
        CHECK(argc==2);
        const std::string_view suite=argv[1];
        std::printf("suite=%s seed=0x%08X libsamplerate=%s\n",argv[1],seed,src_get_version());
        if(suite=="formats") formats();
        else if(suite=="fifo") fifo();
        else if(suite=="src") src_tests();
        else if(suite=="quality") quality();
        else if(suite=="drift") drift();
        else if(suite=="allocation") allocation();
        else if(suite=="mutation-sign") simulate(2000,180);
        else if(suite=="mutation-capacity") capacity_test();
        else CHECK(false);
        std::printf("PASS checks=%llu\n",static_cast<unsigned long long>(checks));
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"FAIL exception: %s\n",e.what()); return 1;
    }
}
