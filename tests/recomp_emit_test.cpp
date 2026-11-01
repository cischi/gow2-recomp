// Wiring of the instructions that patches/xenonrecomp adds to XenonRecomp:
// each encoding is decoded by XenonRecomp's own disassembler and translated by
// its own Recompiler::Recompile. The test checks that
//   1) the instruction is now recognized (Recompile returns true), and
//   2) the operands decoded by binutils (op_str, an independent oracle) land
//      in the right argument of the helper tested in ppc_helpers_test.cpp.
// The semantics are tested there; here only the register mapping matters.
#include "pch.h"
#include "recompiler.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <vector>

namespace {

struct Emitted {
    bool ok;
    std::string name;
    std::string opStr;
    std::string code;
};

Emitted emit(uint32_t word, uint32_t address = 0x82000100) {
    // the disassembler reads guest (big-endian) bytes
    const uint32_t be = ByteSwap(word);
    ppc_insn insn{};
    ppc::Disassemble(&be, 4, address, insn);
    Emitted e{};
    if (insn.opcode == nullptr)
        return e;
    e.name = insn.opcode->name;
    e.opStr = insn.op_str;

    Recompiler recompiler;
    Function fn(0x82000000, 0x1000);
    RecompilerLocalVariables locals;
    CSRState csr = CSRState::Unknown;
    auto switchTable = recompiler.config.switchTables.end();
    e.ok = recompiler.Recompile(fn, address, insn, &be, switchTable, locals, csr);
    e.code = recompiler.out;
    return e;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("vcfpuxws128 -> ppc_vctuxs(vD, vB, UIMM)") {
    auto e = emit(0x1862AA71);
    CHECK(e.name == "vcfpuxws128");
    CHECK(e.opStr == "v3,v53,2");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ppc_vctuxs(ctx.v3, ctx.v53, 2);"));
}

TEST_CASE("vpkuwus128 -> ppc_vpkuwus(vD, vA, vB)") {
    auto e = emit(0x1538FBC1);
    CHECK(e.name == "vpkuwus128");
    CHECK(e.opStr == "v9,v24,v63");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ppc_vpkuwus(ctx.v9, ctx.v24, ctx.v63);"));
}

TEST_CASE("vpkuhus128 -> ppc_vpkuhus(vD, vA, vB)") {
    auto e = emit(0x1429F341);
    CHECK(e.name == "vpkuhus128");
    CHECK(e.opStr == "v1,v9,v62");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ppc_vpkuhus(ctx.v1, ctx.v9, ctx.v62);"));
}

TEST_CASE("vpkswss128 -> ppc_vpkswss(vD, vA, vB)") {
    auto e = emit(0x15202281);
    CHECK(e.name == "vpkswss128");
    CHECK(e.opStr == "v9,v0,v36");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ppc_vpkswss(ctx.v9, ctx.v0, ctx.v36);"));
}

TEST_CASE("vnor128 -> ppc_vnor(vD, vA, vB)") {
    auto e = emit(0x15208291);
    CHECK(e.name == "vnor128");
    CHECK(e.opStr == "v9,v0,v48");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ppc_vnor(ctx.v9, ctx.v0, ctx.v48);"));
}

TEST_CASE("vsel128 -> ppc_vsel(vD, vA, vB, vD)") {
    auto e = emit(0x14438351);
    CHECK(e.name == "vsel128");
    CHECK(e.opStr == "v2,v3,v48,v2");  // the fourth operand is vD itself
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ppc_vsel(ctx.v2, ctx.v3, ctx.v48, ctx.v2);"));
}

TEST_CASE("eqv -> rA = ppc_eqv(rS, rB)") {
    auto e = emit(0x7C611238);
    CHECK(e.name == "eqv");
    CHECK(e.opStr == "r1,r3,r2");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ctx.r1.u64 = ppc_eqv(ctx.r3.u64, ctx.r2.u64);"));
}

TEST_CASE("mulhd -> rT = ppc_mulhd(rA, rB)") {
    auto e = emit(0x7CC14092);
    CHECK(e.name == "mulhd");
    CHECK(e.opStr == "r6,r1,r8");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ctx.r6.s64 = ppc_mulhd(ctx.r1.s64, ctx.r8.s64);"));
}

TEST_CASE("frsqrte -> frD = ppc_frsqrte(frB)") {
    auto e = emit(0xFC002834);
    CHECK(e.name == "frsqrte");
    CHECK(e.opStr == "f0,f5");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ctx.f0.f64 = ppc_frsqrte(ctx.f5.f64);"));
}

TEST_CASE("bdnzt -> ppc_bdnzt(ctr, crN.bit) with the right BI bit") {
    // bdnzt eq (BI = 2 -> cr0.eq), branch backwards by 12 bytes
    auto e = emit(0x4102FFF4, 0x82000100);
    CHECK(e.name == "bdnzt");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "if (ppc_bdnzt(ctx.ctr, ctx.cr0.eq)) goto loc_820000F4;"));

    // BI = 25 -> cr6.gt
    auto g = emit(0x4119FFF4, 0x82000100);
    CHECK(g.name == "bdnzt");
    CHECK(g.opStr.rfind("4*cr6+gt,", 0) == 0);
    REQUIRE(g.ok);
    CHECK(contains(g.code, "if (ppc_bdnzt(ctx.ctr, ctx.cr6.gt)) goto loc_820000F4;"));
}

TEST_CASE("dcbst -> no code, but instruction recognized") {
    auto e = emit(0x7C00486C);
    CHECK(e.name == "dcbst");
    REQUIRE(e.ok);
    CHECK(e.code == "\t// dcbst r0,r9\n");
}

TEST_CASE("record form eqv. stays flagged and is not translated silently") {
    // eqv. r1,r3,r2: XenonRecomp prints "has RC bit enabled but no comparison",
    // which check_recomp_log.py treats as an error.
    auto e = emit(0x7C611239);
    CHECK(e.name == "eqv.");
    CHECK_FALSE(contains(e.code, "cr0"));
}

// ---- D3D pack/unpack e blrl (patches/xenonrecomp/0002) -----------------------
// Words built from the VX128_4 / VX128_3 format (Xenia ppc_instr.h and
// ppc_opcode_table_gen.cc: vpkd3d128 0x18000610, vupkd3d128 0x180007F0) and
// verified with the disassembler's op_str (binutils): these are the instructions
// that GoW2's generated code left as __builtin_debugtrap().

TEST_CASE("vpkd3d128 type 3 (FLOAT16_2) -> ppc_vpkd3d128_float16_2(vD, vB, pack, shift)") {
    auto e = emit(0x1BED0ED4);  // sub_82989D00
    CHECK(e.name == "vpkd3d128");
    CHECK(e.opStr == "v63,v1,3,1,3");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ppc_vpkd3d128_float16_2(ctx.v63, ctx.v1, 1, 3);"));
    CHECK_FALSE(contains(e.code, "debugtrap"));
}

TEST_CASE("vpkd3d128 type 3 with pack 0 stays flagged") {
    auto e = emit(0x1BEC0ED4);  // v63,v1,3,0,3: Xenia assert_unhandled_case(pack)
    CHECK(e.opStr == "v63,v1,3,0,3");
    CHECK(contains(e.code, "__builtin_debugtrap();"));
}

TEST_CASE("vupkd3d128 type 3 (FLOAT16_2) -> ppc_vupkd3d128_float16_2(vD, vB)") {
    auto e = emit(0x1BECFFF5);  // sub_829EA9B0, sub_829EAAD8
    CHECK(e.name == "vupkd3d128");
    CHECK(e.opStr == "v63,v63,12");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ppc_vupkd3d128_float16_2(ctx.v63, ctx.v63);"));
    CHECK_FALSE(contains(e.code, "debugtrap"));
}

TEST_CASE("vupkd3d128 type 5 (FLOAT16_4) -> ppc_vupkd3d128_float16_4(vD, vB)") {
    auto e = emit(0x1BF407F4);  // sub_82BAD510, sub_82BAD5A0
    CHECK(e.name == "vupkd3d128");
    CHECK(e.opStr == "v63,v0,20");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ppc_vupkd3d128_float16_4(ctx.v63, ctx.v0);"));
    CHECK_FALSE(contains(e.code, "debugtrap"));
}

TEST_CASE("blrl -> indirect call to LR, then LR = next address") {
    auto e = emit(0x4E800021, 0x82CD7DA0);  // sub_82CD7D70: mtlr r3; blrl
    CHECK(e.name == "blrl");
    REQUIRE(e.ok);
    CHECK_FALSE(contains(e.code, "debugtrap"));
    // the target is the LR before the update (Xenia InstrEmit_bclrx)
    const auto read = e.code.find(".u32 = ctx.lr;");
    const auto write = e.code.find("ctx.lr = 0x82CD7DA4;");
    const auto call = e.code.find("PPC_CALL_INDIRECT_FUNC(");
    REQUIRE(read != std::string::npos);
    REQUIRE(write != std::string::npos);
    REQUIRE(call != std::string::npos);
    CHECK(read < write);
    CHECK(write < call);
}

// Returns on CTR (patch 0005): bclr/bcctr is unconditional only if BO ignores
// both the CR (bit 0x10) and the CTR (bit 0x04). With bdzlr/bdnzlr the analysis must
// carry on past it, as in the tail of the CRT's memset (mtctr; bdzlr; stb; blr).
namespace {
size_t analyzedSize(std::initializer_list<uint32_t> words) {
    std::vector<uint32_t> be;
    for (uint32_t w : words)
        be.push_back(ByteSwap(w));
    return Function::Analyze(be.data(), be.size() * 4, 0x82000000).size;
}
}  // namespace

TEST_CASE("bdzlr and bdnzlr are conditional returns: the analysis carries on") {
    CHECK(analyzedSize({0x7CA903A6, 0x4E400020, 0x98860001, 0x4E800020}) == 16);  // bdzlr
    CHECK(analyzedSize({0x7CA903A6, 0x4E000020, 0x98860001, 0x4E800020}) == 16);  // bdnzlr
}

TEST_CASE("blr and bctr remain unconditional returns") {
    CHECK(analyzedSize({0x4E800020, 0x98860001, 0x4E800020}) == 4);  // blr
    CHECK(analyzedSize({0x4E800420, 0x98860001, 0x4E800020}) == 4);  // bctr
}

// ---- Instructions found in GoW3 (patches/xenonrecomp/0007) ------------------
// Encodings built from the PowerPC/VMX formats and checked with the
// disassembler's op_str; the vcmpbfp128. and vsldoi128 words come from GoW3's default.xex.

TEST_CASE("bdzf -> decrement CTR, branch if CTR == 0 and the CR bit is clear") {
    auto e = emit(0x4042FFF4, 0x82000100);  // bdzf eq (BI = 2 -> cr0.eq)
    CHECK(e.name == "bdzf");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "--ctx.ctr.u64;"));
    CHECK(contains(e.code, "if (ctx.ctr.u32 == 0 && !ctx.cr0.eq) goto loc_820000F4;"));

    auto g = emit(0x4059FFF4, 0x82000100);  // BI = 25 -> cr6.gt
    CHECK(g.opStr.rfind("4*cr6+gt,", 0) == 0);
    REQUIRE(g.ok);
    CHECK(contains(g.code, "if (ctx.ctr.u32 == 0 && !ctx.cr6.gt) goto loc_820000F4;"));
}

TEST_CASE("subfze, addme, addc -> carry helpers, record forms compare into cr0") {
    auto a = emit(0x7C640190);
    CHECK(a.name == "subfze");
    CHECK(a.opStr == "r3,r4");
    REQUIRE(a.ok);
    CHECK(contains(a.code, "ctx.r3.u64 = ppc_subfze(ctx.r4.u64, ctx.xer);"));

    auto ar = emit(0x7C640191);
    CHECK(ar.name == "subfze.");
    REQUIRE(ar.ok);
    CHECK(contains(ar.code, "ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);"));

    auto m = emit(0x7C6401D4);
    CHECK(m.name == "addme");
    CHECK(m.opStr == "r3,r4");
    REQUIRE(m.ok);
    CHECK(contains(m.code, "ctx.r3.u64 = ppc_addme(ctx.r4.u64, ctx.xer);"));

    auto c = emit(0x7C642814);
    CHECK(c.name == "addc");
    CHECK(c.opStr == "r3,r4,r5");
    REQUIRE(c.ok);
    CHECK(contains(c.code, "ctx.r3.u64 = ppc_addc(ctx.r4.u64, ctx.r5.u64, ctx.xer);"));
}

TEST_CASE("mulhdu -> rT = ppc_mulhdu(rA, rB)") {
    auto e = emit(0x7C642812);
    CHECK(e.name == "mulhdu");
    CHECK(e.opStr == "r3,r4,r5");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "ctx.r3.u64 = ppc_mulhdu(ctx.r4.u64, ctx.r5.u64);"));
}

namespace {
struct UpdateCase {
    uint32_t word;
    const char* name;
    const char* opStr;
    std::vector<std::string> code;  // in this order: rA is written last
};

void checkUpdateCases(std::initializer_list<UpdateCase> cases) {
    for (const auto& c : cases) {
        INFO(c.name);
        auto e = emit(c.word);
        CHECK(e.name == c.name);
        CHECK(e.opStr == c.opStr);
        REQUIRE(e.ok);
        size_t at = 0;
        for (const auto& line : c.code) {
            INFO(line);
            at = e.code.find(line, at);
            CHECK(at != std::string::npos);
        }
    }
}
}  // namespace

TEST_CASE("D-form update loads and stores write the EA back to rA") {
    checkUpdateCases({
        {0xA464FFF8, "lhzu", "r3,-8(r4)", {"ea = -8 + ctx.r4.u32;", "ctx.r3.u64 = PPC_LOAD_U16(ea);", "ctx.r4.u32 = ea;"}},
        {0xAC64FFFE, "lhau", "r3,-2(r4)", {"ea = -2 + ctx.r4.u32;", "ctx.r3.s64 = int16_t(PPC_LOAD_U16(ea));", "ctx.r4.u32 = ea;"}},
        {0xB461FFFE, "sthu", "r3,-2(r1)", {"ea = -2 + ctx.r1.u32;", "PPC_STORE_U16(ea, ctx.r3.u16);", "ctx.r1.u32 = ea;"}},
        {0xC4230004, "lfsu", "f1,4(r3)", {"ea = 4 + ctx.r3.u32;", ".u32 = PPC_LOAD_U32(ea);", "ctx.f1.f64 = double(", "ctx.r3.u32 = ea;"}},
        {0xCC230008, "lfdu", "f1,8(r3)", {"ea = 8 + ctx.r3.u32;", "ctx.f1.u64 = PPC_LOAD_U64(ea);", "ctx.r3.u32 = ea;"}},
        {0xD4230004, "stfsu", "f1,4(r3)", {"ea = 4 + ctx.r3.u32;", ".f32 = float(ctx.f1.f64);", "PPC_STORE_U32(ea, ", "ctx.r3.u32 = ea;"}},
        {0xDC23FFF8, "stfdu", "f1,-8(r3)", {"ea = -8 + ctx.r3.u32;", "PPC_STORE_U64(ea, ctx.f1.u64);", "ctx.r3.u32 = ea;"}},
    });
}

TEST_CASE("X-form update loads and stores write rA + rB back to rA") {
    checkUpdateCases({
        {0x7C642A6E, "lhzux", "r3,r4,r5", {"ea = ctx.r4.u32 + ctx.r5.u32;", "ctx.r3.u64 = PPC_LOAD_U16(ea);", "ctx.r4.u32 = ea;"}},
        {0x7C6428EE, "lbzux", "r3,r4,r5", {"ea = ctx.r4.u32 + ctx.r5.u32;", "ctx.r3.u64 = PPC_LOAD_U8(ea);", "ctx.r4.u32 = ea;"}},
        {0x7C64286E, "lwzux", "r3,r4,r5", {"ea = ctx.r4.u32 + ctx.r5.u32;", "ctx.r3.u64 = PPC_LOAD_U32(ea);", "ctx.r4.u32 = ea;"}},
        {0x7C6429EE, "stbux", "r3,r4,r5", {"ea = ctx.r4.u32 + ctx.r5.u32;", "PPC_STORE_U8(ea, ctx.r3.u8);", "ctx.r4.u32 = ea;"}},
        {0x7C642B6E, "sthux", "r3,r4,r5", {"ea = ctx.r4.u32 + ctx.r5.u32;", "PPC_STORE_U16(ea, ctx.r3.u16);", "ctx.r4.u32 = ea;"}},
        {0x7C242C6E, "lfsux", "f1,r4,r5", {"ea = ctx.r4.u32 + ctx.r5.u32;", ".u32 = PPC_LOAD_U32(ea);", "ctx.f1.f64 = double(", "ctx.r4.u32 = ea;"}},
    });
}

TEST_CASE("lvehx -> the aligned quadword, like lvewx") {
    auto e = emit(0x7C24284E);
    CHECK(e.name == "lvehx");
    CHECK(e.opStr == "v1,r4,r5");
    REQUIRE(e.ok);
    CHECK(contains(e.code, "PPC_HOST(((ctx.r4.u32 + ctx.r5.u32) & ~0xF))"));
}

TEST_CASE("halfword vector shifts and vaddsws -> helpers") {
    auto sra = emit(0x10221B44);
    CHECK(sra.name == "vsrah");
    CHECK(sra.opStr == "v1,v2,v3");
    REQUIRE(sra.ok);
    CHECK(contains(sra.code, "ppc_vsrah(ctx.v1, ctx.v2, ctx.v3);"));

    auto sl = emit(0x10221944);
    CHECK(sl.name == "vslh");
    REQUIRE(sl.ok);
    CHECK(contains(sl.code, "ppc_vslh(ctx.v1, ctx.v2, ctx.v3);"));

    auto sr = emit(0x10221A44);
    CHECK(sr.name == "vsrh");
    REQUIRE(sr.ok);
    CHECK(contains(sr.code, "ppc_vsrh(ctx.v1, ctx.v2, ctx.v3);"));

    auto add = emit(0x10221B80);
    CHECK(add.name == "vaddsws");
    REQUIRE(add.ok);
    CHECK(contains(add.code, "ppc_vaddsws(ctx.v1, ctx.v2, ctx.v3);"));
}

TEST_CASE("vsubshs, vcmpgtsh, vcmpequh -> lane-wise SSE on halfwords, vA op vB") {
    auto s = emit(0x10221F40);
    CHECK(s.name == "vsubshs");
    REQUIRE(s.ok);
    CHECK(contains(s.code, "simde_mm_subs_epi16(simde_mm_load_si128((simde__m128i*)ctx.v2.s16), simde_mm_load_si128((simde__m128i*)ctx.v3.s16))"));

    auto gt = emit(0x10221B46);
    CHECK(gt.name == "vcmpgtsh");
    REQUIRE(gt.ok);
    CHECK(contains(gt.code, "simde_mm_cmpgt_epi16(simde_mm_load_si128((simde__m128i*)ctx.v2.s16), simde_mm_load_si128((simde__m128i*)ctx.v3.s16))"));

    auto eq = emit(0x10221846);
    CHECK(eq.name == "vcmpequh");
    REQUIRE(eq.ok);
    CHECK(contains(eq.code, "simde_mm_cmpeq_epi16(simde_mm_load_si128((simde__m128i*)ctx.v2.u16), simde_mm_load_si128((simde__m128i*)ctx.v3.u16))"));
    CHECK_FALSE(contains(eq.code, "cr6"));

    auto eqr = emit(0x10221C46);
    CHECK(eqr.name == "vcmpequh.");
    REQUIRE(eqr.ok);
    CHECK(contains(eqr.code, "ctx.cr6.setFromMask("));
    CHECK(contains(eqr.code, "0xFFFF"));

    auto gtr = emit(0x10221F46);
    CHECK(gtr.name == "vcmpgtsh.");
    REQUIRE(gtr.ok);
    CHECK(contains(gtr.code, "ctx.cr6.setFromMask("));
    CHECK(contains(gtr.code, "0xFFFF"));
}

TEST_CASE("vspltish -> sign-extended 5-bit immediate in every halfword") {
    auto m = emit(0x103F034C);
    CHECK(m.name == "vspltish");
    CHECK(m.opStr == "v1,-1");
    REQUIRE(m.ok);
    CHECK(contains(m.code, "simde_mm_set1_epi16(short(-1))"));

    auto p = emit(0x102F034C);
    CHECK(p.opStr == "v1,15");
    REQUIRE(p.ok);
    CHECK(contains(p.code, "simde_mm_set1_epi16(short(15))"));
}

TEST_CASE("vcmpbfp128 -> ppc_vcmpbfp; the record form sets cr6 = 0 0 all-in-bounds 0") {
    auto r = emit(0x19AF71E5);  // GoW3 0x82DC3E84
    CHECK(r.name == "vcmpbfp128.");
    REQUIRE(r.ok);
    CHECK_FALSE(contains(r.code, "debugtrap"));
    CHECK(contains(r.code, "ctx.cr6.eq = ppc_vcmpbfp("));
    CHECK(contains(r.code, "ctx.cr6.lt = 0;"));
    CHECK(contains(r.code, "ctx.cr6.gt = 0;"));
    CHECK(contains(r.code, "ctx.cr6.so = 0;"));

    auto n = emit(0x19AF71A5);
    CHECK(n.name == "vcmpbfp128");
    REQUIRE(n.ok);
    CHECK_FALSE(contains(n.code, "debugtrap"));
    CHECK_FALSE(contains(n.code, "cr6"));
    CHECK(contains(n.code, "ppc_vcmpbfp("));

    auto v = emit(0x10221BC6);
    CHECK(v.name == "vcmpbfp");
    CHECK(v.opStr == "v1,v2,v3");
    REQUIRE(v.ok);
    CHECK(contains(v.code, "ppc_vcmpbfp(ctx.v1, ctx.v2, ctx.v3);"));
}

TEST_CASE("VMX128 word once taken for the PPC405 mulchw. decodes as vsldoi128") {
    auto e = emit(0x10C98951);  // GoW3 0x82FDA6A0 (VX128_5: opcode 4, bit 0x10)
    CHECK(e.name == "vsldoi128");
    REQUIRE(e.ok);
}
