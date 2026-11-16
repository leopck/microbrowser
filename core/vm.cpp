#include "microbrowser/vm.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mb {
namespace {

constexpr std::size_t max_operands = 64;
constexpr std::size_t max_call_depth = 16;
constexpr std::size_t max_locals = 32;
constexpr std::size_t max_functions = 128;
constexpr std::size_t max_states = 256;
constexpr std::size_t max_constants = 256;
constexpr std::size_t max_code_bytes = 65'535;
constexpr uint32_t max_event_instructions = 20'000;

enum class Opcode : uint8_t {
    push_const = 1,
    push_bool = 2,
    load_state = 3,
    store_state = 4,
    load_local = 5,
    store_local = 6,
    pop = 7,
    add = 8,
    subtract = 9,
    multiply = 10,
    divide = 11,
    negate = 12,
    equal = 13,
    not_equal = 14,
    less = 15,
    less_equal = 16,
    greater = 17,
    greater_equal = 18,
    logical_not = 19,
    jump = 20,
    jump_false = 21,
    call = 22,
    builtin = 23,
    return_value = 24,
};

struct Instruction {
    Opcode opcode = Opcode::push_const;
    std::size_t pc = 0;
    std::size_t next_pc = 0;
    std::size_t target_pc = 0;
    uint16_t index = 0;
    uint8_t immediate = 0;
    uint8_t argument_count = 0;
};

[[nodiscard]] std::string location(const Function& function, std::size_t pc) {
    return "script error in function '" + function.name + "' at pc " + std::to_string(pc) + ": ";
}

[[noreturn]] void fail(const Function& function, std::size_t pc, const std::string& message) {
    throw ScriptError(location(function, pc) + message);
}

[[nodiscard]] std::string_view opcode_name(Opcode opcode) {
    switch (opcode) {
    case Opcode::push_const: return "PUSH_CONST";
    case Opcode::push_bool: return "PUSH_BOOL";
    case Opcode::load_state: return "LOAD_STATE";
    case Opcode::store_state: return "STORE_STATE";
    case Opcode::load_local: return "LOAD_LOCAL";
    case Opcode::store_local: return "STORE_LOCAL";
    case Opcode::pop: return "POP";
    case Opcode::add: return "ADD";
    case Opcode::subtract: return "SUBTRACT";
    case Opcode::multiply: return "MULTIPLY";
    case Opcode::divide: return "DIVIDE";
    case Opcode::negate: return "NEGATE";
    case Opcode::equal: return "EQUAL";
    case Opcode::not_equal: return "NOT_EQUAL";
    case Opcode::less: return "LESS";
    case Opcode::less_equal: return "LESS_EQUAL";
    case Opcode::greater: return "GREATER";
    case Opcode::greater_equal: return "GREATER_EQUAL";
    case Opcode::logical_not: return "NOT";
    case Opcode::jump: return "JUMP";
    case Opcode::jump_false: return "JUMP_FALSE";
    case Opcode::call: return "CALL";
    case Opcode::builtin: return "BUILTIN";
    case Opcode::return_value: return "RETURN";
    }
    return "UNKNOWN";
}

[[nodiscard]] uint16_t read_u16_immediate(const std::vector<uint8_t>& code, std::size_t offset) {
    return static_cast<uint16_t>(code[offset]) |
           static_cast<uint16_t>(static_cast<uint16_t>(code[offset + 1]) << 8U);
}

void require_immediate_bytes(const Function& function, std::size_t pc, std::size_t instruction_size,
                             Opcode opcode) {
    if (pc >= function.code.size() || instruction_size > function.code.size() - pc) {
        fail(function, pc, "truncated " + std::string(opcode_name(opcode)) + " instruction");
    }
}

Instruction decode_instruction(const Function& function, std::size_t pc) {
    if (pc >= function.code.size()) {
        fail(function, pc, "instruction pointer is outside the function");
    }
    const uint8_t raw_opcode = function.code[pc];
    if (raw_opcode < static_cast<uint8_t>(Opcode::push_const) ||
        raw_opcode > static_cast<uint8_t>(Opcode::return_value)) {
        fail(function, pc, "unknown opcode " + std::to_string(raw_opcode));
    }

    Instruction result;
    result.opcode = static_cast<Opcode>(raw_opcode);
    result.pc = pc;
    std::size_t size = 1;
    switch (result.opcode) {
    case Opcode::push_const:
    case Opcode::load_state:
    case Opcode::store_state:
    case Opcode::load_local:
    case Opcode::store_local:
    case Opcode::jump:
    case Opcode::jump_false:
        size = 3;
        break;
    case Opcode::push_bool:
        size = 2;
        break;
    case Opcode::call:
        size = 4;
        break;
    case Opcode::builtin:
        size = 3;
        break;
    default:
        break;
    }
    require_immediate_bytes(function, pc, size, result.opcode);
    result.next_pc = pc + size;

    switch (result.opcode) {
    case Opcode::push_const:
    case Opcode::load_state:
    case Opcode::store_state:
    case Opcode::load_local:
    case Opcode::store_local:
        result.index = read_u16_immediate(function.code, pc + 1);
        break;
    case Opcode::push_bool:
        result.immediate = function.code[pc + 1];
        break;
    case Opcode::jump:
    case Opcode::jump_false: {
        const uint16_t bits = read_u16_immediate(function.code, pc + 1);
        const int32_t displacement = bits <= INT16_MAX ? static_cast<int32_t>(bits)
                                                       : static_cast<int32_t>(bits) - 65'536;
        const int64_t target = static_cast<int64_t>(result.next_pc) + displacement;
        if (target < 0 || target >= static_cast<int64_t>(function.code.size())) {
            fail(function, pc, "jump target is outside the function");
        }
        result.target_pc = static_cast<std::size_t>(target);
        break;
    }
    case Opcode::call:
        result.index = read_u16_immediate(function.code, pc + 1);
        result.argument_count = function.code[pc + 3];
        break;
    case Opcode::builtin:
        result.immediate = function.code[pc + 1];
        result.argument_count = function.code[pc + 2];
        break;
    default:
        break;
    }
    return result;
}

[[nodiscard]] unsigned builtin_arity(uint8_t id) {
    switch (id) {
    case 0:
    case 1:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 19:
    case 20:
    case 25:
    case 10:
    case 11:
    case 26:
    case 27:
    case 28:
        return 2;
    case 2:
    case 18:
        return 3;
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 12:
    case 21:
    case 22:
    case 23:
    case 24:
        return 1;
    case 8:
        return 0;
    case 9:
        return 4;
    default:
        return 0;
    }
}

[[nodiscard]] Value::Type builtin_result_type(uint8_t id) {
    switch (id) {
    case 7:
    case 8:
    case 9:
    case 12:
    case 13:
    case 15:
    case 19:
    case 24:
    case 25:
        return Value::Type::void_value;
    case 16:
    case 28:
        return Value::Type::boolean;
    case 20:
    case 23:
    case 27:
        return Value::Type::string;
    default:
        return Value::Type::number;
    }
}

[[nodiscard]] std::string_view value_type_name(Value::Type type) {
    switch (type) {
    case Value::Type::number: return "number";
    case Value::Type::boolean: return "boolean";
    case Value::Type::string: return "string";
    case Value::Type::void_value: return "void";
    }
    return "invalid";
}

void require_scalar(const Value& value, const Function& function, std::size_t pc,
                    std::string_view description) {
    if (value.type != Value::Type::number && value.type != Value::Type::boolean &&
        value.type != Value::Type::string) {
        fail(function, pc, std::string(description) + " is not a scalar value");
    }
    if (value.type == Value::Type::number && !std::isfinite(value.number)) {
        fail(function, pc, std::string(description) + " is not finite");
    }
}

void validate_function(const Package& package, const Function& function) {
    if (function.local_count > max_locals) {
        fail(function, 0, "local count exceeds 32");
    }
    if (function.parameter_count > function.local_count) {
        fail(function, 0, "parameter count exceeds local count");
    }
    if (function.constants.size() > max_constants) {
        fail(function, 0, "constant count exceeds 256");
    }
    if (function.code.size() > max_code_bytes) {
        fail(function, 0, "code size exceeds 65,535 bytes");
    }
    for (const Value& constant : function.constants) {
        require_scalar(constant, function, 0, "constant");
    }

    std::vector<uint8_t> boundaries(function.code.size() + 1, 0);
    std::vector<Instruction> jumps;
    std::size_t pc = 0;
    while (pc < function.code.size()) {
        const Instruction instruction = decode_instruction(function, pc);
        boundaries[pc] = 1;
        switch (instruction.opcode) {
        case Opcode::push_const:
            if (instruction.index >= function.constants.size()) {
                fail(function, pc, "constant index is out of range");
            }
            break;
        case Opcode::load_state:
        case Opcode::store_state:
            if (instruction.index >= package.states.size()) {
                fail(function, pc, "state index is out of range");
            }
            break;
        case Opcode::load_local:
        case Opcode::store_local:
            if (instruction.index >= function.local_count) {
                fail(function, pc, "local index is out of range");
            }
            break;
        case Opcode::push_bool:
            if (instruction.immediate > 1) {
                fail(function, pc, "PUSH_BOOL immediate must be zero or one");
            }
            break;
        case Opcode::call:
            if (instruction.index >= package.functions.size()) {
                fail(function, pc, "function index is out of range");
            }
            if (instruction.argument_count != package.functions[instruction.index].parameter_count) {
                fail(function, pc, "CALL argument count does not match the target function arity");
            }
            break;
        case Opcode::builtin: {
            if (instruction.immediate > 28) {
                fail(function, pc, "unknown builtin ID " + std::to_string(instruction.immediate));
            }
            const unsigned arity = builtin_arity(instruction.immediate);
            if (instruction.argument_count != arity) {
                fail(function, pc, "BUILTIN argument count for " +
                                       std::to_string(instruction.immediate) + " expected " +
                                       std::to_string(arity) + ", got " + std::to_string(instruction.argument_count));
            }
            break;
        }
        case Opcode::jump:
        case Opcode::jump_false:
            jumps.push_back(instruction);
            break;
        default:
            break;
        }
        pc = instruction.next_pc;
    }
    for (const Instruction& jump : jumps) {
        if (boundaries[jump.target_pc] == 0) {
            fail(function, jump.pc, "jump target is not on an instruction boundary");
        }
    }
}

void validate_package_for_execution(const Package& package) {
    if (package.states.size() > max_states) {
        throw ScriptError("script package has more than 256 state fields");
    }
    if (package.functions.size() > max_functions) {
        throw ScriptError("script package has more than 128 functions");
    }
    for (const StateField& state : package.states) {
        Function context;
        context.name = "package state '" + state.name + "'";
        require_scalar(state.initial, context, 0, "initial state");
    }
    for (const Function& function : package.functions) {
        validate_function(package, function);
    }
}

Value make_number(float number) {
    Value result;
    result.type = Value::Type::number;
    result.number = number;
    return result;
}


Value make_boolean(bool boolean) {
    Value result;
    result.type = Value::Type::boolean;
    result.boolean = boolean;
    return result;
}

Value make_void() {
    Value result;
    result.type = Value::Type::void_value;
    return result;
}

struct Frame {
    const Function* function = nullptr;
    std::size_t pc = 0;
    std::vector<Value> locals;
    std::vector<Value> operands;
};

Frame make_frame(const Function& function, std::span<const Value> arguments) {
    if (arguments.size() != function.parameter_count) {
        throw ScriptError("function '" + function.name + "' expects " +
                          std::to_string(function.parameter_count) + " arguments, received " +
                          std::to_string(arguments.size()));
    }
    Frame frame;
    frame.function = &function;
    frame.locals.resize(function.local_count);
    frame.operands.reserve(max_operands);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        require_scalar(arguments[index], function, 0, "function argument");
        frame.locals[index] = arguments[index];
    }
    return frame;
}

void push_operand(Frame& frame, Value value, std::size_t pc) {
    if (frame.operands.size() == max_operands) {
        fail(*frame.function, pc, "operand stack exceeds 64 values");
    }
    frame.operands.push_back(std::move(value));
}

Value pop_operand(Frame& frame, std::size_t pc) {
    if (frame.operands.empty()) {
        fail(*frame.function, pc, "operand stack underflow");
    }
    Value result = std::move(frame.operands.back());
    frame.operands.pop_back();
    return result;
}

std::vector<Value> pop_arguments(Frame& frame, uint8_t count, std::size_t pc) {
    if (frame.operands.size() < count) {
        fail(*frame.function, pc, "operand stack has too few call arguments");
    }
    std::vector<Value> arguments(count);
    for (std::size_t index = count; index > 0; --index) {
        arguments[index - 1] = pop_operand(frame, pc);
        require_scalar(arguments[index - 1], *frame.function, pc, "call argument");
    }
    return arguments;
}

template <typename Operation>
auto host_call(Operation&& operation, const Function& function, std::size_t pc,
               std::string_view description) -> decltype(operation()) {
    const std::string prefix = location(function, pc) + std::string(description) + ": ";
    try {
        return operation();
    } catch (const NavigationSignal&) {
        throw;
    } catch (const ScriptError& error) {
        throw ScriptError(prefix + error.what());
    } catch (const std::exception& error) {
        throw ScriptError(prefix + error.what());
    } catch (...) {
        throw ScriptError(prefix + "host threw a non-standard exception");
    }
}

Value read_host_state(VmHost& host, uint32_t index, const Package& package,
                      const Function& function, std::size_t pc) {
    Value value = host_call([&]() { return host.read_state(index); }, function, pc,
                            "reading state '" + package.states[index].name + "'");
    require_scalar(value, function, pc, "value read from state");
    if (value.type != package.states[index].initial.type) {
        fail(function, pc, "value read from state '" + package.states[index].name +
                               "' has type " + std::string(value_type_name(value.type)) +
                               " instead of " +
                               std::string(value_type_name(package.states[index].initial.type)));
    }
    return value;
}

Value make_numeric_result(float number, const Function& function, std::size_t pc,
                          std::string_view operation) {
    if (!std::isfinite(number)) {
        fail(function, pc, std::string(operation) + " produced a non-finite number");
    }
    return make_number(number);
}

Value evaluate_add(Value left, Value right, const Function& function, std::size_t pc) {
    if (left.type == Value::Type::number && right.type == Value::Type::number) {
        return make_numeric_result(left.number + right.number, function, pc, "ADD");
    }
    if (left.type == Value::Type::string && right.type == Value::Type::string) {
        left.string.append(right.string);
        return left;
    }
    fail(function, pc, "ADD requires two numbers or two strings");
}

Value evaluate_binary_numeric(Opcode opcode, const Value& left, const Value& right,
                              const Function& function, std::size_t pc) {
    if (left.type != Value::Type::number || right.type != Value::Type::number) {
        fail(function, pc, std::string(opcode_name(opcode)) + " requires two numbers");
    }
    switch (opcode) {
    case Opcode::subtract:
        return make_numeric_result(left.number - right.number, function, pc, "SUBTRACT");
    case Opcode::multiply:
        return make_numeric_result(left.number * right.number, function, pc, "MULTIPLY");
    case Opcode::divide:
        if (right.number == 0.0F) {
            fail(function, pc, "division by zero");
        }
        return make_numeric_result(left.number / right.number, function, pc, "DIVIDE");
    case Opcode::less:
        return make_boolean(left.number < right.number);
    case Opcode::less_equal:
        return make_boolean(left.number <= right.number);
    case Opcode::greater:
        return make_boolean(left.number > right.number);
    case Opcode::greater_equal:
        return make_boolean(left.number >= right.number);
    default:
        fail(function, pc, "internal numeric opcode mismatch");
    }
}

Value evaluate_equality(Opcode opcode, const Value& left, const Value& right,
                        const Function& function, std::size_t pc) {
    if (left.type != right.type) {
        fail(function, pc, "equality operands must have matching types");
    }
    bool equal = false;
    switch (left.type) {
    case Value::Type::number:
        equal = left.number == right.number;
        break;
    case Value::Type::boolean:
        equal = left.boolean == right.boolean;
        break;
    case Value::Type::string:
        equal = left.string == right.string;
        break;
    case Value::Type::void_value:
        fail(function, pc, "equality does not accept void operands");
    }
    return make_boolean(opcode == Opcode::equal ? equal : !equal);
}

Value finish_return(std::vector<Frame>& frames) {
    Frame& frame = frames.back();
    Value result = frame.operands.empty() ? make_void() : pop_operand(frame, frame.pc);
    frames.pop_back();
    return result;
}

} // namespace

Value execute_script(const Package& package, uint32_t function_id,
                     std::span<const Value> arguments, VmHost& host,
                     uint32_t instruction_budget) {
    validate_package_for_execution(package);
    if (function_id >= package.functions.size()) {
        throw ScriptError("script function index " + std::to_string(function_id) + " is out of range");
    }

    std::vector<Frame> frames;
    frames.reserve(max_call_depth);
    frames.push_back(make_frame(package.functions[function_id], arguments));
    uint32_t instructions_left = std::min(instruction_budget, max_event_instructions);

    while (true) {
        Frame& frame = frames.back();
        const Function& function = *frame.function;
        const std::size_t pc = frame.pc;
        if (pc >= function.code.size()) {
            fail(function, pc, "reached the end of code without RETURN");
        }
        if (instructions_left == 0) {
            fail(function, pc, "instruction budget exhausted");
        }
        --instructions_left;
        const Instruction instruction = decode_instruction(function, pc);
        frame.pc = instruction.next_pc;

        switch (instruction.opcode) {
        case Opcode::push_const:
            push_operand(frame, function.constants[instruction.index], pc);
            break;
        case Opcode::push_bool:
            push_operand(frame, make_boolean(instruction.immediate != 0), pc);
            break;
        case Opcode::load_state:
            push_operand(frame, read_host_state(host, instruction.index, package, function, pc), pc);
            break;
        case Opcode::store_state: {
            Value value = pop_operand(frame, pc);
            require_scalar(value, function, pc, "state assignment");
            const Value::Type expected = package.states[instruction.index].initial.type;
            if (value.type != expected) {
                fail(function, pc, "state assignment has type " +
                                       std::string(value_type_name(value.type)) + " instead of " +
                                       std::string(value_type_name(expected)));
            }
            host_call([&]() { host.write_state(instruction.index, value); }, function, pc,
                      "writing state '" + package.states[instruction.index].name + "'");
            break;
        }
        case Opcode::load_local:
            if (instruction.index >= frame.locals.size()) {
                fail(function, pc, "local index is out of range");
            }
            push_operand(frame, frame.locals[instruction.index], pc);
            break;
        case Opcode::store_local:
            frame.locals[instruction.index] = pop_operand(frame, pc);
            break;
        case Opcode::pop:
            static_cast<void>(pop_operand(frame, pc));
            break;
        case Opcode::add: {
            Value right = pop_operand(frame, pc);
            Value left = pop_operand(frame, pc);
            push_operand(frame, evaluate_add(std::move(left), std::move(right), function, pc), pc);
            break;
        }
        case Opcode::subtract:
        case Opcode::multiply:
        case Opcode::divide:
        case Opcode::less:
        case Opcode::less_equal:
        case Opcode::greater:
        case Opcode::greater_equal: {
            Value right = pop_operand(frame, pc);
            Value left = pop_operand(frame, pc);
            push_operand(frame,
                         evaluate_binary_numeric(instruction.opcode, left, right, function, pc), pc);
            break;
        }
        case Opcode::negate: {
            Value value = pop_operand(frame, pc);
            if (value.type != Value::Type::number) {
                fail(function, pc, "NEGATE requires a number");
            }
            push_operand(frame, make_numeric_result(-value.number, function, pc, "NEGATE"), pc);
            break;
        }
        case Opcode::equal:
        case Opcode::not_equal: {
            Value right = pop_operand(frame, pc);
            Value left = pop_operand(frame, pc);
            push_operand(frame, evaluate_equality(instruction.opcode, left, right, function, pc), pc);
            break;
        }
        case Opcode::logical_not: {
            Value value = pop_operand(frame, pc);
            if (value.type != Value::Type::boolean) {
                fail(function, pc, "NOT requires a boolean");
            }
            push_operand(frame, make_boolean(!value.boolean), pc);
            break;
        }
        case Opcode::jump:
            frame.pc = instruction.target_pc;
            break;
        case Opcode::jump_false: {
            Value condition = pop_operand(frame, pc);
            if (condition.type != Value::Type::boolean) {
                fail(function, pc, "JUMP_FALSE requires a boolean condition");
            }
            if (!condition.boolean) {
                frame.pc = instruction.target_pc;
            }
            break;
        }
        case Opcode::call: {
            std::vector<Value> call_arguments = pop_arguments(frame, instruction.argument_count, pc);
            if (frames.size() == max_call_depth) {
                fail(function, pc, "call depth exceeds 16 active functions");
            }
            if (frame.operands.size() == max_operands) {
                fail(function, pc, "no operand stack space for the call result");
            }
            frames.push_back(make_frame(package.functions[instruction.index], call_arguments));
            break;
        }
        case Opcode::builtin: {
            std::vector<Value> call_arguments = pop_arguments(frame, instruction.argument_count, pc);
            Value result = host_call(
                [&]() { return host.builtin(instruction.immediate, call_arguments); }, function, pc,
                "builtin " + std::to_string(instruction.immediate));
            const Value::Type expected = builtin_result_type(instruction.immediate);
            if (result.type != expected) {
                fail(function, pc, "builtin " + std::to_string(instruction.immediate) + " returned " +
                                       std::string(value_type_name(result.type)) + " instead of " +
                                       std::string(value_type_name(expected)));
            }
            if (result.type == Value::Type::number && !std::isfinite(result.number)) {
                fail(function, pc, "builtin returned a non-finite number");
            }
            push_operand(frame, std::move(result), pc);
            break;
        }
        case Opcode::return_value: {
            const std::size_t callee_pc = pc;
            Value result = finish_return(frames);
            if (frames.empty()) {
                return result;
            }
            push_operand(frames.back(), std::move(result), callee_pc);
            break;
        }
        }
    }
}

} // namespace mb
