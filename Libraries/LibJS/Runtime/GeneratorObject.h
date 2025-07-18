/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class JS_API GeneratorObject : public Object {
    JS_OBJECT(GeneratorObject, Object);
    GC_DECLARE_ALLOCATOR(GeneratorObject);

public:
    static ThrowCompletionOr<GC::Ref<GeneratorObject>> create(Realm&, Value, ECMAScriptFunctionObject*, NonnullOwnPtr<ExecutionContext>);
    virtual ~GeneratorObject() override = default;
    void visit_edges(Cell::Visitor&) override;

    struct IterationResult {
        IterationResult() = delete;
        explicit IterationResult(Value value, bool done)
            : done(done)
            , value(value)
        {
        }

        bool done { false };
        Value value;
    };

    ThrowCompletionOr<IterationResult> resume(VM&, Value value, Optional<StringView> const& generator_brand);
    ThrowCompletionOr<IterationResult> resume_abrupt(VM&, JS::Completion abrupt_completion, Optional<StringView> const& generator_brand);

    enum class GeneratorState {
        SuspendedStart,
        SuspendedYield,
        Executing,
        Completed,
    };
    GeneratorState generator_state() const { return m_generator_state; }
    void set_generator_state(GeneratorState generator_state) { m_generator_state = generator_state; }

protected:
    GeneratorObject(Realm&, Object& prototype, NonnullOwnPtr<ExecutionContext>, Optional<StringView> generator_brand = {});

    ThrowCompletionOr<GeneratorState> validate(VM&, Optional<StringView> const& generator_brand);
    virtual ThrowCompletionOr<IterationResult> execute(VM&, JS::Completion const& completion);

private:
    NonnullOwnPtr<ExecutionContext> m_execution_context;
    GC::Ptr<ECMAScriptFunctionObject> m_generating_function;
    Value m_previous_value;
    GeneratorState m_generator_state { GeneratorState::SuspendedStart };
    Optional<StringView> m_generator_brand;
};

}
