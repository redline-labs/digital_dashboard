#pragma once

// Prints a compiled tape as readable C++, for inspecting what csym generates:
//
//   csym::emit(std::cout, Fn::Program<csym::Mode::Jacobian>::tape, "my_function");

#include <cstdio>
#include <ostream>
#include <string>

#include "csym/lower.h"

namespace csym {

namespace detail {
inline const char* iop_name(IOp op) {
  switch (op) {
    case IOp::Sqrt: return "std::sqrt";
    case IOp::Pow: return "std::pow";
    case IOp::Sin: return "std::sin";
    case IOp::Cos: return "std::cos";
    case IOp::Tan: return "std::tan";
    case IOp::Asin: return "std::asin";
    case IOp::Acos: return "std::acos";
    case IOp::Atan: return "std::atan";
    case IOp::Exp: return "std::exp";
    case IOp::Log: return "std::log";
    case IOp::Tanh: return "std::tanh";
    case IOp::Abs: return "std::abs";
    case IOp::Floor: return "std::floor";
    case IOp::Atan2: return "std::atan2";
    case IOp::Min: return "std::min";
    case IOp::Max: return "std::max";
    case IOp::Sign: return "sign";
    case IOp::SignNoZero: return "sign_no_zero";
    // Printed as operators by emit(), never by name.
    case IOp::In:
    case IOp::Const:
    case IOp::Add:
    case IOp::Sub:
    case IOp::Mul:
    case IOp::Div:
    case IOp::Neg:
    case IOp::Lt:
    case IOp::Le:
    case IOp::Eq:
    case IOp::Where: return "?";
  }
  return "?";
}
}  // namespace detail

template <class TapeT>
void emit(std::ostream& os, const TapeT& t, const std::string& name = "f") {
  auto reg = [](std::uint32_t i) { return "_t" + std::to_string(i); };
  auto num = [](double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return std::string(buf);
  };
  os << "// " << t.op_count() << " ops\n";
  os << "void " << name << "(const double* in, double* out) {\n";
  std::size_t seg = 0;
  for (std::size_t i = 0; i < t.instrs.size(); ++i) {
    if (seg < t.segments.size() && t.segments[seg].begin == i) {
      if (t.segments.size() > 1) os << "  // needed by output groups 0x" << std::hex << t.segments[seg].mask << std::dec << "\n";
      ++seg;
    }
    const Instr& k = t.instrs[i];
    os << "  const double " << reg(static_cast<std::uint32_t>(i)) << " = ";
    const std::string a = reg(k.a), b = reg(k.b), c = reg(k.c);
    switch (k.op) {
      case IOp::In: os << "in[" << k.a << "]"; break;
      case IOp::Const: os << num(t.consts[k.a]); break;
      case IOp::Add: os << a << " + " << b; break;
      case IOp::Sub: os << a << " - " << b; break;
      case IOp::Mul: os << a << " * " << b; break;
      case IOp::Div: os << a << " / " << b; break;
      case IOp::Neg: os << "-" << a; break;
      case IOp::Lt: os << "(" << a << " < " << b << ")"; break;
      case IOp::Le: os << "(" << a << " <= " << b << ")"; break;
      case IOp::Eq: os << "(" << a << " == " << b << ")"; break;
      case IOp::Where: os << a << " != 0 ? " << b << " : " << c; break;
      case IOp::Atan2:
      case IOp::Min:
      case IOp::Max:
      case IOp::Pow: os << detail::iop_name(k.op) << "(" << a << ", " << b << ")"; break;
      case IOp::Sqrt:
      case IOp::Sin:
      case IOp::Cos:
      case IOp::Tan:
      case IOp::Asin:
      case IOp::Acos:
      case IOp::Atan:
      case IOp::Exp:
      case IOp::Log:
      case IOp::Tanh:
      case IOp::Abs:
      case IOp::Sign:
      case IOp::SignNoZero:
      case IOp::Floor: os << detail::iop_name(k.op) << "(" << a << ")"; break;
    }
    os << ";\n";
  }
  for (std::size_t j = 0; j < t.outputs.size(); ++j) os << "  out[" << j << "] = " << reg(t.outputs[j]) << ";\n";
  os << "}\n";
}

}  // namespace csym
