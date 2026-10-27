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
