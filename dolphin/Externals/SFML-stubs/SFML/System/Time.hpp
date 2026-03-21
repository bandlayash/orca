#pragma once
#include <chrono>
namespace sf
{
using Time = std::chrono::microseconds;
inline Time milliseconds(int amount) { return std::chrono::milliseconds(amount); }
inline Time seconds(float amount) { return std::chrono::duration_cast<Time>(std::chrono::duration<float>(amount)); }
} // namespace sf
