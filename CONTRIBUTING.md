# Code Contribution Guidelines

We welcome the use of AI tools (such as Copilot, ChatGPT, or Claude) as productivity accelerators. However, AI tool usage **does not alter or reduce the standards for code quality, safety, and contributor accountability**. The following policies must always be adhered to.

## 1. Scope & Long-Term Maintainability
* Just because code can be written doesn't mean it belongs in the repository. We only accept changes that maintainers are willing and able to maintain long-term.
* All user-facing changes must adhere to sound UI/UX principles, fit intuitively within the existing workflow, and avoid unnecessary visual or functional clutter.
* If you have an idea for a major change or new feature, please [join our Discord](https://discord.gg/erSUx3v4YH) to discuss it before opening a Pull Request.

## 2. 100% Code Ownership & Fluency
* **You are responsible for every line:** You must fully understand, be able to explain, and defend all logic and architectural choices in your submitted Pull Request.
* **No raw AI dumps:** Submitting unvetted or unverified AI output is prohibited. Maintainers are here to review design and safety, not to act as human debuggers or QA testers for AI code.
* **Review responsiveness:** If a review reveals that you do not understand the mechanics of your submitted code or cannot debug issues identified in review, the PR will be closed.

## 3. Atomic Scope & Size Limits
* Keep PRs tightly focused on a single logical change, bug fix, or feature.
* Generating massive diffs in seconds does not mean maintainers can review them in seconds. PRs that bundle large sweeps of AI-generated refactoring, introduce multi-subsystem churn, or exceed manageable review sizes will be closed without line-by-line inspection.

## 4. Local Verification Required
Before marking any PR as ready for review, you must:
* Manually test your change on all platforms which may have distinct behavior under the change. So for example if you touched code within a ‘#if ANDROID’ block we need you to have run it on Android.
* Isolate and fix root causes for any bugs introduced by the changes.
* Ensure strict adherence to project code style, formatting standards, and build system requirements.

## 5. Use Concise, Human-Authored PR Descriptions
* **Focus on intent:** Describe what changed and why, rather than providing line-by-line summaries of code diffs.
* **No raw AI summaries:** Avoid pasting raw AI-generated PR descriptions. They are often padded with unnecessary prose, obvious mechanical details, and bloated explanations.
* **Refining is fine, generating isn't:** Using AI tools to edit, polish, or translate your own summary is acceptable, but the original logic and context must come directly from you.