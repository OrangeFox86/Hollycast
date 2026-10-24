## AI-Assisted Code Contributions

We welcome the use of AI tools (such as Copilot, ChatGPT, or Claude) as productivity accelerators. However, AI tool usage **does not alter or reduce the standards for code quality, safety, and contributor accountability**.

If you use AI assistance when drafting code for this project, you must adhere to the following policies:

### 1. 100% Code Ownership & Fluency
* **You are responsible for every line:** You must fully understand, be able to explain, and defend all logic, memory handling, and architectural choices in your submitted Pull Request.
* **No raw AI dumps:** Submitting unvetted or unverified AI output is prohibited. Maintainers are here to review design and safety, not to act as human debuggers or QA testers for AI code.
* **Review responsiveness:** If a review reveals that you do not understand the mechanics of your submitted code or cannot debug issues identified in review, the PR will be closed.

### 2. Atomic Scope & Size Limits
* Keep PRs tightly focused on a single logical change, bug fix, or feature.
* Generating massive diffs in seconds does not mean maintainers can review them in seconds. PRs that bundle large sweeps of AI-generated refactoring, introduce multi-subsystem churn, or exceed manageable review sizes will be closed without line-by-line inspection.

### 3. Local Verification Required
Before marking any PR as ready for review, you must:
* Manually test your change on all platforms which may have distinct behavior under the change. So for example if you touched code within a ‘#if ANDROID’ block we need you to have run it on Android.
* Isolate and fix root causes for any bugs introduced by the changes.
* Ensure strict adherence to project code style, formatting standards, and build system requirements.