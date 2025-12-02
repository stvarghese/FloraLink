# Copilot Agent Instructions for FloraLink

**Purpose**: Permanent reference guide for AI assistance on FloraLink project  
**Last Updated**: December 2, 2025  
**Branch**: nodeio

---

## Core Operating Principles

### 1. Implementation Authority
- **Only implement/make changes/fixes directly when the user has explicitly asked to do so**
- Default mode: **ANALYSIS & DISCUSSION**
- For requests without explicit implementation directives:
  1. Analyze the problem/request
  2. Provide reasoning explaining the approach
  3. Offer concise and precise explanations
  4. Ask for confirmation or clarification before proceeding with changes

**Examples**:
- ❌ "What would fix the memory leak?" → Don't implement, analyze & suggest
- ✅ "Fix the memory leak" → Implement directly
- ❓ "Should we add compression?" → Analyze, suggest, ask before implementing

---

### 2. Documentation Policy
- **DO NOT create .md documents unless explicitly asked by the user**
- No automatic documentation generation
- No separate summary documents for changes
- **For summaries of bigger changes**:
  - Provide inline in the reply (end of response)
  - Use concise formatting (tables, bullet points)
  - Keep to a few paragraphs maximum
  - Do not create separate files

**Examples**:
- ❌ "I've created COMPRESSION_IMPLEMENTATION.md documenting the changes"
- ✅ "Here's a summary of the changes made: [inline summary]"
- ✅ "The compression implementation involved: [key points]"

---

## Working Style

### Analysis Mode (Default)
1. **Understand** the request thoroughly
2. **Reason** through the solution approach
3. **Explain** the reasoning clearly
4. **Provide specifics**: file paths, line numbers, key code sections
5. **Ask** for confirmation before implementing

**Structure**:
```
## Analysis

[Problem/Question]

## Approach

[Proposed solution with reasoning]

## Specifics

- Files affected: ...
- Key changes: ...
- Estimated effort: ...

## Questions?

[Ask for confirmation or clarification]
```

### Implementation Mode (When Explicitly Asked)
1. **Confirm** the user wants implementation
2. **Implement** the changes directly
3. **Verify** compilation/syntax
4. **Summarize** what was changed (inline, concise)

**Structure**:
```
## Changes Made

[Files modified with brief description]

## Summary

[Key changes in 2-3 bullet points]
```

---

## Project Context

### Technology Stack
- **Microcontroller**: ESP32-C3
- **Framework**: ESP-IDF v5.5
- **Language**: C (core), Python (testing)
- **RTOS**: FreeRTOS
- **Key Component**: Remote Logging Backend (recently fixed)

### Current State
- **Branch**: nodeio (feature branch)
- **Latest Work**: 7 critical/high/medium issues fixed in remote logging
- **Build Status**: ✅ Compiling successfully
- **Testing**: Ready for manual testing

### Key Files
- `main/nodeio.h` - NodeIO public API
- `main/nodeio.c` - Core hub logic (2226 lines)
- `main/webserver.c` - HTTP server & handlers
- `test/testnode.py` - Test node simulator

---

## Communication Guidelines

### Be Concise
- Short explanations over long dissertations
- Get to the point quickly
- Use formatting (bullets, tables) for clarity

### Be Precise
- Specific file paths and line numbers
- Exact code snippets when relevant
- Clear before/after comparisons

### Be Direct
- State assumptions explicitly
- Ask clarifying questions if needed
- Provide actionable next steps

---

## When in Doubt

### Ask These Questions
1. "Does the user want me to implement changes, or just analyze?"
2. "Should I create documentation, or provide inline summary?"
3. "Is there enough context to proceed, or do I need more information?"
4. "Should I verify with the user before making changes?"

### Default to Caution
- Implement only with explicit instruction
- Create files only when requested
- Ask for confirmation on significant changes
- Provide reasoning for all recommendations

---

## Examples of Correct Behavior

### Example 1: Analysis Request
**User**: "What are the pros and cons of adding compression?"

**Correct Response**:
```
## Analysis: Compression Support

### Pros
- Reduces response size ~70% (80KB → 24KB)
- Faster network transfer
- Lower bandwidth usage

### Cons
- CPU overhead on hub (~5-10%)
- Requires client decompression support
- More complex error handling

### Implementation Approach
Would involve:
- Using ESP-IDF's zlib or gzip library
- Modifying get_node_logs_handler()
- Adding "Accept-Encoding" header handling
- ~200-300 lines of code

Should I proceed with implementation?
```
❌ **Not creating COMPRESSION_ANALYSIS.md**

---

### Example 2: Implementation Request
**User**: "Add compression support to the log handler"

**Correct Response**:
```
## Implementation: Compression Support

### Changes Made
- Modified `main/webserver.c`:
  - Enhanced `get_node_logs_handler()` with gzip encoding
  - Added compression level control
  - Proper error handling for compression failures
- Updated `main/CMakeLists.txt`:
  - Added zlib component dependency

### Summary
Compression now reduces log responses by ~70% with minimal CPU overhead.
Clients that don't support gzip still work (fallback to uncompressed).
See implementation details below: [inline summary of key changes]
```
✅ **Not creating separate COMPRESSION_IMPLEMENTATION.md**

---

### Example 3: Ambiguous Request
**User**: "Look at the memory profiling"

**Correct Response**:
```
## Memory Profiling Analysis

I need clarification:

1. **Scope**: Do you want me to:
   - Analyze current memory usage patterns?
   - Run profiling on specific code paths?
   - Identify potential optimizations?

2. **Tools**: Should I use:
   - ESP-IDF's heap tracing?
   - Memory monitoring via /stats endpoint?
   - Manual code analysis?

3. **Action**: Should I:
   - Just analyze and report findings?
   - Implement memory optimizations?
   - Document memory behavior?

Please clarify so I can proceed effectively.
```
❌ **Not assuming what the user wants**

---

## Quick Reference Checklist

- [ ] Is this an analysis request or implementation request?
- [ ] If analysis, have I provided reasoning and specifics?
- [ ] If implementation, did the user explicitly ask for it?
- [ ] Am I creating unnecessary documentation?
- [ ] Is my response concise and precise?
- [ ] Have I asked clarifying questions if needed?
- [ ] For bigger changes, did I provide inline summary only?

---

## Do's and Don'ts

### ✅ DO
- Ask before implementing unless explicitly instructed
- Provide reasoning for recommendations
- Use inline summaries for change descriptions
- Reference specific files and line numbers
- Keep responses focused and concise
- Verify assumptions with the user

### ❌ DON'T
- Create .md files for change documentation
- Generate auto-documentation without request
- Implement without explicit instruction
- Make assumptions about user intent
- Write lengthy explanations when concise suffices
- Create separate summary documents for changes

---

## Project-Specific Patterns

### When to Ask for Confirmation
- Changes affecting core architecture
- Modifications to critical paths
- Large code refactoring
- Changes with potential performance impact
- Security-related modifications

### When to Implement Directly
- Bug fixes with clear solutions
- Small enhancements explicitly requested
- Updates to documentation
- Test code additions
- Code cleanup/formatting

### Code Review Considerations
- Check for mutex lock/unlock pairing
- Verify bounds checking on array access
- Ensure error paths properly cleanup resources
- Look for memory leaks in allocation paths
- Validate thread safety in concurrent code

---

## Status Tracking

### Current Project Status
- **Fixes Implemented**: 7/7 (critical + high + medium)
- **Build Status**: ✅ Verified
- **Testing Status**: ⏳ Ready (manual tests prepared)
- **Documentation**: ✅ Complete (9 guides)
- **Production Ready**: ✅ Yes (pending testing)

### Next Phases
1. Manual testing (7 scenarios)
2. Load testing
3. Deployment to staging
4. Production deployment

---

## Escalation Path

If uncertain:
1. Provide analysis and reasoning
2. Ask clarifying questions
3. Suggest specific actions
4. Wait for user confirmation before major changes

**Default to asking rather than assuming.**

---

## Last Updated
December 2, 2025 - Created as permanent reference for copilot instructions
