/**
 * @file lns_formats.cpp
 * @brief Parsing of the LNS extension instruction formats (custom-0 opcode).
 */

#include "assembler/parser.h"
#include "common/instructions.h"
#include "processor/registers.h"
#include "utils/utils.h"

#include <string>

namespace Kites
{
namespace
{
/// Canonical raw name ("l10") for an LNS register token such as "la0" or "l10".
std::string canonicalLnsName(const std::string &token)
{
    return LnsRegisterName(static_cast<size_t>(LnsRegisterIndex(token)));
}
} // namespace

// ladd / lsub / lmul / ldiv: lrd, lrs1, lrs2
bool Parser::parse_O_LNS_C_LNS_C_LNS()
{
    if (peekToken(1).line_number == currentToken().line_number &&
        peekToken(1).type == TokenType::LNS_REGISTER &&
        peekToken(2).line_number == currentToken().line_number &&
        peekToken(2).type == TokenType::COMMA &&
        peekToken(3).line_number == currentToken().line_number &&
        peekToken(3).type == TokenType::LNS_REGISTER &&
        peekToken(4).line_number == currentToken().line_number &&
        peekToken(4).type == TokenType::COMMA &&
        peekToken(5).line_number == currentToken().line_number &&
        peekToken(5).type == TokenType::LNS_REGISTER &&
        (peekToken(6).type == TokenType::EOF_ ||
         peekToken(6).line_number != currentToken().line_number))
    {
        ICUnit block;
        block.setOpcode(currentToken().value);
        block.setLineNumber(currentToken().line_number);
        block.setInstructionIndex(instruction_index_);
        block.setRd(canonicalLnsName(peekToken(1).value));
        block.setRs1(canonicalLnsName(peekToken(3).value));
        block.setRs2(canonicalLnsName(peekToken(5).value));
        skipCurrentLine();
        intermediate_code_.emplace_back(block, true);
        instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
        instruction_index_++;
        return true;
    }
    return false;
}

// fcvt.lns.s: lrd, frs1   (rs2 field is fixed to 0)
bool Parser::parse_O_LNS_C_FPR()
{
    if (peekToken(1).line_number == currentToken().line_number &&
        peekToken(1).type == TokenType::LNS_REGISTER &&
        peekToken(2).line_number == currentToken().line_number &&
        peekToken(2).type == TokenType::COMMA &&
        peekToken(3).line_number == currentToken().line_number &&
        peekToken(3).type == TokenType::FP_REGISTER &&
        (peekToken(4).type == TokenType::EOF_ ||
         peekToken(4).line_number != currentToken().line_number))
    {
        ICUnit block;
        block.setOpcode(currentToken().value);
        block.setLineNumber(currentToken().line_number);
        block.setInstructionIndex(instruction_index_);
        block.setRd(canonicalLnsName(peekToken(1).value));
        block.setRs1(reg_alias_to_name.at(peekToken(3).value));
        skipCurrentLine();
        intermediate_code_.emplace_back(block, true);
        instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
        instruction_index_++;
        return true;
    }
    return false;
}

// fcvt.s.lns: frd, lrs1   (rs2 field is fixed to 0)
bool Parser::parse_O_FPR_C_LNS()
{
    if (peekToken(1).line_number == currentToken().line_number &&
        peekToken(1).type == TokenType::FP_REGISTER &&
        peekToken(2).line_number == currentToken().line_number &&
        peekToken(2).type == TokenType::COMMA &&
        peekToken(3).line_number == currentToken().line_number &&
        peekToken(3).type == TokenType::LNS_REGISTER &&
        (peekToken(4).type == TokenType::EOF_ ||
         peekToken(4).line_number != currentToken().line_number))
    {
        ICUnit block;
        block.setOpcode(currentToken().value);
        block.setLineNumber(currentToken().line_number);
        block.setInstructionIndex(instruction_index_);
        block.setRd(reg_alias_to_name.at(peekToken(1).value));
        block.setRs1(canonicalLnsName(peekToken(3).value));
        skipCurrentLine();
        intermediate_code_.emplace_back(block, true);
        instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
        instruction_index_++;
        return true;
    }
    return false;
}

// fcvt.lns.w / .wu / .l / .lu: lrd, rs1   (rs2 field = integer type, from the encoding table)
bool Parser::parse_O_LNS_C_GPR()
{
    if (peekToken(1).line_number == currentToken().line_number &&
        peekToken(1).type == TokenType::LNS_REGISTER &&
        peekToken(2).line_number == currentToken().line_number &&
        peekToken(2).type == TokenType::COMMA &&
        peekToken(3).line_number == currentToken().line_number &&
        peekToken(3).type == TokenType::GP_REGISTER &&
        (peekToken(4).type == TokenType::EOF_ ||
         peekToken(4).line_number != currentToken().line_number))
    {
        ICUnit block;
        block.setOpcode(currentToken().value);
        block.setLineNumber(currentToken().line_number);
        block.setInstructionIndex(instruction_index_);
        block.setRd(canonicalLnsName(peekToken(1).value));
        block.setRs1(reg_alias_to_name.at(peekToken(3).value));
        skipCurrentLine();
        intermediate_code_.emplace_back(block, true);
        instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
        instruction_index_++;
        return true;
    }
    return false;
}

// fcvt.w.lns / .wu / .l / .lu: rd, lrs1   (rs2 field = integer type, from the encoding table)
bool Parser::parse_O_GPR_C_LNS()
{
    if (peekToken(1).line_number == currentToken().line_number &&
        peekToken(1).type == TokenType::GP_REGISTER &&
        peekToken(2).line_number == currentToken().line_number &&
        peekToken(2).type == TokenType::COMMA &&
        peekToken(3).line_number == currentToken().line_number &&
        peekToken(3).type == TokenType::LNS_REGISTER &&
        (peekToken(4).type == TokenType::EOF_ ||
         peekToken(4).line_number != currentToken().line_number))
    {
        ICUnit block;
        block.setOpcode(currentToken().value);
        block.setLineNumber(currentToken().line_number);
        block.setInstructionIndex(instruction_index_);
        block.setRd(reg_alias_to_name.at(peekToken(1).value));
        block.setRs1(canonicalLnsName(peekToken(3).value));
        skipCurrentLine();
        intermediate_code_.emplace_back(block, true);
        instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
        instruction_index_++;
        return true;
    }
    return false;
}

// llw lrd, imm(rs1)  /  lsw lrs2, imm(rs1)
bool Parser::parse_O_LNS_C_I_LP_GPR_RP()
{
    if (peekToken(1).line_number == currentToken().line_number &&
        peekToken(1).type == TokenType::LNS_REGISTER &&
        peekToken(2).line_number == currentToken().line_number &&
        peekToken(2).type == TokenType::COMMA &&
        peekToken(3).line_number == currentToken().line_number &&
        peekToken(3).type == TokenType::NUM &&
        peekToken(4).line_number == currentToken().line_number &&
        peekToken(4).type == TokenType::LPAREN &&
        peekToken(5).line_number == currentToken().line_number &&
        peekToken(5).type == TokenType::GP_REGISTER &&
        peekToken(6).line_number == currentToken().line_number &&
        peekToken(6).type == TokenType::RPAREN &&
        (peekToken(7).type == TokenType::EOF_ ||
         peekToken(7).line_number != currentToken().line_number))
    {
        ICUnit block;
        block.setOpcode(currentToken().value);
        block.setLineNumber(currentToken().line_number);
        block.setInstructionIndex(instruction_index_);

        const int64_t imm = std::stoll(peekToken(3).value);
        if (imm < -2048 || imm > 2047)
        {
            recordError(ParseError(peekToken(3).line_number, "Immediate value out of range"));
            errors_.all_errors.emplace_back(errors::ImmediateOutOfRangeError(
                "Immediate value out of range", "Expected: -2048 <= imm <= 2047", filename_,
                peekToken(3).line_number, peekToken(3).column_number,
                sourceLine(peekToken(3).line_number)));
            skipCurrentLine();
            return true;
        }
        block.setImm(std::to_string(imm));

        if (instruction_set::isValidLnsLoadInstruction(block.getOpcode()))
            block.setRd(canonicalLnsName(peekToken(1).value));
        else
            block.setRs2(canonicalLnsName(peekToken(1).value));
        block.setRs1(reg_alias_to_name.at(peekToken(5).value));

        skipCurrentLine();
        intermediate_code_.emplace_back(block, true);
        instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
        instruction_index_++;
        return true;
    }
    return false;
}
} // namespace Kites
