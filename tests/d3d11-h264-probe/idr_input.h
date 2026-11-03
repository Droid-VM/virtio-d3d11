#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>

// Deliberately only the bundled progressive baseline, one-slice IDR contract.
struct Bits {
    std::vector<uint8_t> data;
    size_t bit = 0;
    explicit Bits(const std::vector<uint8_t>& nal) {
        unsigned zeros = 0;
        for (size_t i = 1; i < nal.size(); ++i) {
            uint8_t b = nal[i];
            if (zeros == 2 && b == 3) {
                if (i + 1 == nal.size() || nal[i + 1] > 3) throw std::runtime_error("bad EPB");
                zeros = 0; continue;
            }
            data.push_back(b); zeros = b == 0 ? zeros + 1 : 0;
        }
    }
    uint32_t u(unsigned n) {
        if (n > 32 || bit + n > data.size() * 8) throw std::runtime_error("truncated RBSP");
        uint32_t v = 0;
        while (n--) { v = (v << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1); ++bit; }
        return v;
    }
    uint32_t ue() {
        unsigned z = 0;
        while (!u(1)) if (++z > 30) throw std::runtime_error("Exp-Golomb overflow");
        return ((1u << z) - 1) + u(z);
    }
    int se() { unsigned v = ue(); return v & 1 ? int((v + 1) / 2) : -int(v / 2); }
    void expect(unsigned got, unsigned want) {
        if (got != want) throw std::runtime_error("unsupported fixture syntax/value");
    }
};
struct IdrInput {
    std::vector<uint8_t> sps, pps, slice, bitstream;
    unsigned slice_offset = 0;
    unsigned wmb, hmb, refs, logfn, poc, logpoc = 0, direct;
    unsigned l0, l1, deblock, constrained, redundant;
    int qp, qs, chroma, slice_qp, alpha = 0, beta = 0;
    unsigned frame_num, idr_id, poc_lsb = 0, disable_deblock = 0;
};
inline size_t start(const std::vector<uint8_t>& a, size_t p) {
    for (; p + 3 <= a.size(); ++p)
        if (!a[p] && !a[p+1] && (a[p+2] == 1 ||
            (p + 4 <= a.size() && !a[p+2] && a[p+3] == 1))) return p;
    return a.size();
}
inline IdrInput parse_idr(const std::vector<uint8_t>& a) {
    if (a.empty() || a.size() > 65536 || start(a, 0) != 0) throw std::runtime_error("bad Annex-B size/start");
    IdrInput x{};
    for (size_t p = 0; p < a.size();) {
        size_t b = p + (a[p+2] == 1 ? 3 : 4), e = start(a, b);
        if (b >= e) throw std::runtime_error("empty NAL");
        std::vector<uint8_t> n(a.begin()+b, a.begin()+e);
        unsigned t = n[0] & 31;
        if (n[0] & 128) throw std::runtime_error("forbidden_zero_bit");
        auto* dst = t == 7 ? &x.sps : t == 8 ? &x.pps : t == 5 ? &x.slice : nullptr;
        if (!dst || !dst->empty()) throw std::runtime_error("requires exactly SPS PPS IDR");
        if ((t == 7 && (!x.pps.empty() || !x.slice.empty())) ||
            (t == 8 && (x.sps.empty() || !x.slice.empty())) ||
            (t == 5 && (x.sps.empty() || x.pps.empty()))) throw std::runtime_error("NAL order");
        *dst = n; p = e;
    }
    if (x.sps.empty() || x.pps.empty() || x.slice.empty()) throw std::runtime_error("missing NAL");
    Bits s(x.sps);
    s.expect(s.u(8),66); s.expect(s.u(8),192); s.expect(s.u(8),10); s.expect(s.ue(),0);
    x.logfn=s.ue(); if (x.logfn>12) throw std::runtime_error("frame_num width");
    x.poc=s.ue(); if (x.poc==0) { x.logpoc=s.ue(); if(x.logpoc>12) throw std::runtime_error("POC width"); }
    else s.expect(x.poc,2);
    x.refs=s.ue(); if (x.refs>1) throw std::runtime_error("DPB size");
    s.expect(s.u(1),0); x.wmb=s.ue(); x.hmb=s.ue(); s.expect(x.wmb,5); s.expect(x.hmb,5);
    s.expect(s.u(1),1); x.direct=s.u(1); s.expect(s.u(1),0);
    // The VUI is not decoder picture state. Byte-identical SPS/PPS to the
    // independently decoded fixture are required by main before any API calls.
    Bits p(x.pps);
    p.expect(p.ue(),0); p.expect(p.ue(),0); p.expect(p.u(1),0); p.expect(p.u(1),0);
    p.expect(p.ue(),0); x.l0=p.ue(); x.l1=p.ue(); p.expect(x.l0,0); p.expect(x.l1,0);
    p.expect(p.u(1),0); p.expect(p.u(2),0);
    x.qp=p.se(); x.qs=p.se(); x.chroma=p.se();
    if(x.qp < -26 || x.qp >25 || x.qs < -26 || x.qs >25 || x.chroma < -12 || x.chroma >12)
        throw std::runtime_error("PPS QP range");
    x.deblock=p.u(1); x.constrained=p.u(1); x.redundant=p.u(1); p.expect(x.redundant,0);
    // Baseline PPS has no extension: require rbsp_stop_one_bit and zero padding.
    p.expect(p.u(1),1); while(p.bit<p.data.size()*8) p.expect(p.u(1),0);
    Bits v(x.slice);
    if (!(x.slice[0]&96)) throw std::runtime_error("IDR is not reference NAL");
    v.expect(v.ue(),0); unsigned type=v.ue(); if(type!=2 && type!=7) throw std::runtime_error("not I slice");
    v.expect(v.ue(),0); x.frame_num=v.u(x.logfn+4); v.expect(x.frame_num,0); x.idr_id=v.ue();
    if(x.poc==0) x.poc_lsb=v.u(x.logpoc+4);
    v.expect(v.u(1),0); v.expect(v.u(1),0); // IDR no_output_of_prior_pics, long_term_reference
    x.slice_qp=v.se(); if(26+x.qp+x.slice_qp<0 || 26+x.qp+x.slice_qp>51) throw std::runtime_error("slice QP");
    if(x.deblock) {
        x.disable_deblock=v.ue(); if(x.disable_deblock>2) throw std::runtime_error("deblock idc");
        if(x.disable_deblock!=1) { x.alpha=v.se(); x.beta=v.se();
            if(x.alpha < -6 || x.alpha>6 || x.beta < -6 || x.beta>6) throw std::runtime_error("deblock offset"); }
    }
    // Preserve original SPS/PPS: production then uses in-AU parameters rather
    // than reconstructing an SPS from DXVA's intentionally incomplete fields.
    for (const auto* nal : {&x.sps, &x.pps, &x.slice}) {
        if (nal == &x.slice) x.slice_offset = unsigned(x.bitstream.size());
        if (nal == &x.slice) x.bitstream.insert(x.bitstream.end(), {0,0,1});
        else x.bitstream.insert(x.bitstream.end(), {0,0,0,1}); // Annex-B SPS/PPS zero_byte
        x.bitstream.insert(x.bitstream.end(), nal->begin(), nal->end());
    }
    return x;
}
