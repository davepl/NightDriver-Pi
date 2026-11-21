#include "testpattern.h"
#include <cassert>
#include <iostream>

class Output : public DisplayOutput
{
public:
    std::atomic<std::sig_atomic_t> stop{0};
    std::vector<std::vector<CRGB>> images;
    std::vector<CRGB> current;
    int Width() const override { return 512; }
    int Height() const override { return 32; }
    std::chrono::microseconds RefreshInterval() const override { return std::chrono::milliseconds(5); }
    void Present(std::span<const CRGB> frame) override
    {
        current.assign(frame.begin(),frame.end());
        Refresh();
    }
    void Refresh() override
    {
        images.push_back(current);
        if (images.size() == 6) stop=1;
    }
};

static bool Equal(const std::vector<CRGB> & a, const std::vector<CRGB> & b)
{
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](auto x,auto y)
        { return x.r==y.r && x.g==y.g && x.b==y.b; });
}

int main()
{
    auto zero=DisplayTest::Render(512,32,0,30,60);
    auto one=DisplayTest::Render(512,32,1,30,60);
    assert(zero.size()==16384 && !Equal(zero,one));
    // Independent top/bottom codes agree on the least-significant bit.
    for (int y : {1,28})
    {
        const size_t at=size_t(y)*512+184+15*8;
        assert(zero[at].g==0 && one[at].g==160);
    }
    // A fully cyan bit pattern includes the highest and lowest code cells.
    auto all=DisplayTest::Render(512,32,65535,60,60);
    assert(all[512+184].g==160 && all[512+184+15*8].g==160);
    for (int fps : {30,60})
    {
        Output out;
        out.Present(DisplayTest::Render(512,32,0,fps,60));
        DisplayTest::Run(out,fps,60,out.stop);
        assert(out.images.size()==6);
        for (size_t i=1;i<out.images.size();++i)
            assert(Equal(out.images[i],out.images[i-1]) == (fps==30 && i%2==1));
    }
    std::cout << "test-pattern frame IDs, binary codes, 30/60 repeats and 60/60 motion tests passed\n";
}
