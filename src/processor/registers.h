/**
 * @file registers.h
 * @brief Contains the definition of the RegisterFile class for managing registers in the simulator.
 * @author Vishank Singh, https://github.com/VishankSingh
 */

#ifndef REGISTERS_H
#define REGISTERS_H

#include "processor/lns/lns_format.h"
#include <QObject>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Kites
{
/**
 * @brief Represents a register file containing integer, floating-point, and vector registers.
 */
class RegisterFile : public QObject
{
    Q_OBJECT
  private:
    static constexpr size_t NUM_GPR = 32; ///< Number of General-Purpose Registers (GPR).
    static constexpr size_t NUM_FPR = 32; ///< Number of Floating-Point Registers (FPR).

    std::array<uint64_t, NUM_GPR> gpr_ = {}; ///< Array for storing GPR values.
    std::array<uint64_t, NUM_FPR> fpr_ = {}; ///< Array for storing FPR values.

    static constexpr size_t NUM_LNS = 32; ///< Number of LNS registers (L0-L31).

    /// LNS register values: 32-bit LNS words. L0 is hard-wired to +0 and every other register
    /// resets to +0 (lns::kPosZero), since the all-zero word would be +1.0 in this format.
    std::array<uint32_t, NUM_LNS> lns_ = {};

    static constexpr size_t NUM_CSR = 4096; ///< Number of Control and Status Registers (CSR).

    std::array<uint64_t, NUM_CSR> csr_ = {}; ///< Array for storing CSR values.

  public:
    /**
     * @brief Enum representing the type of a register.
     */
    enum class RegisterType
    {
        INVALID,        ///< Invalid register type.
        INTEGER,        ///< General-purpose integer register.
        FLOATING_POINT, ///< Floating-point register.
        VECTOR,         ///< Vector register.
        CSR,            ///< Control and Status Register (CSR).
        LNS             ///< Logarithmic number system register (L0-L31).
    };

    explicit RegisterFile(QObject *parent = nullptr);
    virtual ~RegisterFile() = default;

    void Reset();

    /**
     * @brief Reads the value of a General-Purpose Register (GPR).
     * @param reg The index of the GPR to read.
     * @return The value of the GPR at the specified index.
     */
    [[nodiscard]] uint64_t ReadGpr(size_t reg) const;

    /**
     * @brief Writes a value to a General-Purpose Register (GPR).
     * @param reg The index of the GPR to write.
     * @param value The value to write.
     */
    void WriteGpr(size_t reg, uint64_t value);

    /**
     * @brief Reads the value of a Floating-Point Register (FPR).
     * @param reg The index of the FPR to read.
     * @return The value of the FPR at the specified index.
     */
    [[nodiscard]] uint64_t ReadFpr(size_t reg) const;

    /**
     * @brief Writes a value to a Floating-Point Register (FPR).
     * @param reg The index of the FPR to write.
     * @param value The value to write.
     */
    void WriteFpr(size_t reg, uint64_t value);

    /**
     * @brief Reads an LNS register (L0-L31). L0 always reads +0.
     * @param reg The index of the LNS register to read.
     * @return The 32-bit LNS word stored in the register.
     */
    [[nodiscard]] uint32_t ReadLns(size_t reg) const;

    /**
     * @brief Writes an LNS register (L0-L31). Writes to L0 are ignored.
     * @param reg The index of the LNS register to write.
     * @param value The 32-bit LNS word to write (stored as given, not canonicalized).
     */
    void WriteLns(size_t reg, uint32_t value);

    [[nodiscard]] uint64_t ReadCsr(size_t reg) const;

    void WriteCsr(size_t reg, uint64_t value);

    /**
     * @brief Retrieves the values of all General-Purpose Registers (GPR).
     * @return A vector containing the values of all GPRs.
     */
    [[nodiscard]] std::vector<uint64_t> GetGprValues() const;

    /**
     * @brief Retrieves the values of all Floating-Point Registers (FPR).
     * @return A vector containing the values of all FPRs.
     */
    [[nodiscard]] std::vector<uint64_t> GetFprValues() const;

    /**
     * @brief Retrieves the values of all LNS registers.
     * @return A vector containing the 32-bit words of L0-L31.
     */
    [[nodiscard]] std::vector<uint32_t> GetLnsValues() const;

    void ModifyRegister(const std::string &reg_name, uint64_t value);

  signals:
    void updateRegister(size_t regIndex, uint64_t value);
    void updateFRegister(size_t regIndex, uint64_t value);
    void updateLRegister(size_t regIndex, uint32_t value);
    void registerResetSignal();
};

extern const std::unordered_set<std::string> valid_general_purpose_registers;

extern const std::unordered_set<std::string> valid_floating_point_registers;

/// Valid LNS register names: raw (l0-l31) and ABI (lzero, lt0-lt10, ls0-ls11, la0-la7).
extern const std::unordered_set<std::string> valid_lns_registers;

extern const std::unordered_set<std::string> valid_csr_registers;

extern const std::unordered_map<std::string, int> csr_to_address;

/**
 * @brief Map of register aliases to their actual names.
 */
extern const std::unordered_map<std::string, std::string> reg_alias_to_name;

bool IsValidGeneralPurposeRegister(const std::string &reg);

bool IsValidFloatingPointRegister(const std::string &reg);

bool IsValidLnsRegister(const std::string &reg);

/**
 * @brief Maps an LNS register name (raw "l5" or ABI "la0") to its index.
 * @return The index 0-31, or -1 if the name is not an LNS register.
 */
int LnsRegisterIndex(const std::string &reg);

/// Raw name of an LNS register, e.g. "l10". @throws std::out_of_range for index >= 32.
std::string LnsRegisterName(size_t index);

/// ABI name of an LNS register, e.g. "la0" for index 10. @throws std::out_of_range for index >= 32.
std::string LnsRegisterAbiName(size_t index);

bool IsValidCsr(const std::string &reg);
}//namespace Kites
#endif // REGISTERS_H