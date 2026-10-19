#pragma once

#include <QString>

namespace mtg::ai {

// Tool-using agent prompt for capable models (Claude, GPT-class, DeepSeek).
QString agentSystemPrompt(bool vision);

// Single-shot planner prompt for small/local models: one JSON plan per
// request, constrained by the plan schema, with worked examples.
QString guidedSystemPrompt();

}  // namespace mtg::ai
