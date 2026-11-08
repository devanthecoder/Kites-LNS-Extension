#include "registermodel.h"
#include "common/register_names.h"
#include "processor/lns/lns_format.h"
#include <QApplication>
#include <QBrush>
#include <QDebug>
#include <QPalette>
#include <cmath>

namespace Kites
{
namespace
{
// Decimal view of an LNS word: sign * 2^(log / 2^22), with the special values spelled out.
QString lnsDecimalString(uint32_t word)
{
    if (lns::isNaN(word))
        return QString("nan");
    if (lns::isInf(word))
        return QString(lns::signOf(word) ? "-inf" : "inf");
    if (lns::isZero(word))
        return QString("0");
    const double magnitude = std::exp2(static_cast<double>(lns::fieldOf(word)) / lns::kScale);
    return QString::number(lns::signOf(word) ? -magnitude : magnitude, 'g', 8);
}
} // namespace

RegisterModel::RegisterModel(QObject *parent, RegisterFile *regfile) : QAbstractTableModel(parent)
{
    m_currentRegisterFile = regfile;
    connect(m_currentRegisterFile, &RegisterFile::updateRegister, this,
            &RegisterModel::updateRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::updateFRegister, this,
            &RegisterModel::updateFRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::updateLRegister, this,
            &RegisterModel::updateLRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::registerResetSignal, this,
            &RegisterModel::registerResetSlot);
}

int RegisterModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 96; // x0-x31, f0-f31, l0-l31
}

int RegisterModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 3;
} // Register name, alias, value

void RegisterModel::setDisplayBase(Base base)
{
    beginResetModel();
    m_displayBase = base;
    endResetModel();
}

QVariant RegisterModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant{};

    if (orientation == Qt::Horizontal)
    {
        switch (section)
        {
        case 0:
            return QString("Register");
        case 1:
            return QString("Alias");
        case 2:
            return QString("Value");
        }
    }

    return QVariant{};
}

QVariant RegisterModel::data(const QModelIndex &index, int role) const
{
    if (!m_currentRegisterFile)
        return QVariant{};

    if (role == Qt::TextAlignmentRole)
    {
        return Qt::AlignCenter;
    }
    if (role == Qt::BackgroundRole)
    {
        if (static_cast<size_t>(index.row()) == m_highlightedRegisterIndex)
        {
            return QBrush(QColor("#b13232fd")); // Highlight color for the updated register
        }
    }
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
    {
        size_t row = static_cast<size_t>(index.row());
        switch (index.column())
        {
        case 0: // Register Name
            if (row < 32)
            {
                return QString("x%1").arg(row);
            }
            else if (row < 64)
            {
                return QString("f%1").arg(row - 32);
            }
            else
            {
                return QString("l%1").arg(row - 64);
            }
        case 1: // Register Alias
        {
            if (row < 32)
                return QString(register_names::kGprAliases[row]);
            else if (row < 64)
                return QString(register_names::kFprAliases[row - 32]);
            else if (row < 96)
                return QString(register_names::kLnsAliases[row - 64]);
            else
                return QString("");
        }
        case 2: // Register Value
        {
            if (row < 32)
            {
                uint64_t value = m_currentRegisterFile->ReadGpr(row);
                switch (m_displayBase)
                {
                case Base::Binary:
                    return QString("0b%1").arg(QString::number(value, 2));
                case Base::Decimal:
                    return QString::number(static_cast<int64_t>(value));
                case Base::Hexadecimal:
                    return QString("0x%1").arg(QString::number(value, 16).toUpper());
                }
            }
            else if (row < 64)
            {
                int fIndex = row - 32;

                // read raw bits of FP register (you must have this function)
                uint64_t raw = m_currentRegisterFile->ReadFpr(fIndex);

                switch (m_displayBase)
                {
                case Base::Binary:
                {
                    return QString("0b%1").arg(QString::number(raw, 2));
                }

                case Base::Decimal:
                {
                    uint32_t upper32 = static_cast<uint32_t>(raw >> 32);
                    if (upper32 == 0)
                    {
                        // Single-precision float (F-extension): value in lower 32 bits
                        uint32_t lower32 = static_cast<uint32_t>(raw & 0xFFFFFFFF);
                        float fval;
                        memcpy(&fval, &lower32, sizeof(float));
                        return QString::number(static_cast<double>(fval), 'g', 8);
                    }
                    else
                    {
                        // Double-precision (D-extension): full 64 bits
                        double dval;
                        memcpy(&dval, &raw, sizeof(double));
                        return QString::number(dval, 'g', 16);
                    }
                }

                case Base::Hexadecimal:
                {
                    return QString("0x%1").arg(QString::number(raw, 16).toUpper());
                }
                }
            }
            else if (row < 96)
            {
                const uint32_t word = m_currentRegisterFile->ReadLns(row - 64);

                switch (m_displayBase)
                {
                case Base::Binary:
                    return QString("0b%1").arg(QString::number(word, 2));
                case Base::Decimal:
                    return lnsDecimalString(word);
                case Base::Hexadecimal:
                    return QString("0x%1").arg(
                        QString::number(word, 16).toUpper().rightJustified(8, '0'));
                }
            }
            else
            {
                return QString("");
            }
        }
        default:
            return QVariant{};
        }
    }
    return QVariant{};
}

void RegisterModel::changeRegisterFile(RegisterFile *regfile)
{
    beginResetModel();
    m_currentRegisterFile = regfile;
    connect(m_currentRegisterFile, &RegisterFile::updateRegister, this,
            &RegisterModel::updateRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::updateFRegister, this,
            &RegisterModel::updateFRegisterValue);
    connect(m_currentRegisterFile, &RegisterFile::updateLRegister, this,
            &RegisterModel::updateLRegisterValue);
    // maybe we can directly connect it to
    connect(m_currentRegisterFile, &RegisterFile::registerResetSignal, this,
            &RegisterModel::registerResetSlot);
    endResetModel();
}

void RegisterModel::registerResetSlot()
{
    beginResetModel();
    endResetModel();
}

void RegisterModel::updateRegisterValue(size_t regIndex, uint64_t value)
{
    //QModelIndex index = this->index(static_cast<int>(regIndex), 2);
    m_highlightedRegisterIndex = regIndex; // Update highlighted register index
    // emit dataChanged(index,index);
    beginResetModel();
    endResetModel();
}

void RegisterModel::updateFRegisterValue(size_t regIndex, uint64_t value)
{
    //QModelIndex index = this->index(static_cast<int>(regIndex + 32), 2);
    m_highlightedRegisterIndex = regIndex + 32; // Update highlighted register index
    // emit dataChanged(index,index);
    beginResetModel();
    endResetModel();
}

void RegisterModel::updateLRegisterValue(size_t regIndex, uint32_t value)
{
    Q_UNUSED(value);
    m_highlightedRegisterIndex = regIndex + 64; // Update highlighted register index
    beginResetModel();
    endResetModel();
}
} // namespace Kites
