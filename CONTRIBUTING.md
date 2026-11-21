# Contributing Guidelines

Thank you for your interest in contributing to our project. Whether it's a bug report, new feature, correction, or additional
documentation, we greatly value feedback and contributions from our community.

Please read through this document before submitting any issues or pull requests to ensure we have all the necessary
information to effectively respond to your bug report or contribution.


## Reporting Bugs/Feature Requests

We welcome you to use the GitHub issue tracker to report bugs or suggest features.

When filing an issue, please check existing open, or recently closed, issues to make sure somebody else hasn't already
reported the issue. Please try to include as much information as you can. Details like these are incredibly useful:

* A reproducible test case or series of steps
* The version of our code being used, and the DuckDB version and platform (`PRAGMA version;` and `PRAGMA platform;`)
* Any modifications you've made relevant to the bug
* Anything unusual about your environment or deployment

Leave credentials out of issues. Connection errors name the target (an endpoint or a cluster, and the account for a
workgroup), the database, the region, and the profile or secret, so replace those if you'd rather not share them.


## Contributing via Pull Requests
Contributions via pull requests are much appreciated. Before sending us a pull request, please ensure that:

1. You are working against the latest source on the *main* branch.
2. You check existing open, and recently merged, pull requests to make sure someone else hasn't addressed the problem already.
3. You open an issue to discuss any significant work - we would hate for your time to be wasted.

To send us a pull request, please:

1. Fork the repository.
2. Modify the source; please focus on the specific change you are contributing. If you also reformat all the code, it will be hard for us to focus on your change.
3. Ensure local tests pass.
4. If your change affects users, say in the pull request what changed for them (see [Release notes](#release-notes)).
5. Commit to your fork using clear commit messages.
6. Send us a pull request, answering any default questions in the pull request interface.
7. Stay involved in the conversation.

GitHub provides additional document on [forking a repository](https://help.github.com/articles/fork-a-repo/) and
[creating a pull request](https://help.github.com/articles/creating-a-pull-request/).


## How a pull request reaches this repository

This repository is published from the maintainers' internal source tree, one commit per release, so pull requests are
not merged on GitHub. Once a maintainer has reviewed and accepted yours, they re-apply its change in that tree, where it
goes through the same code review and tests as every other change, with a `Co-authored-by:` trailer that names you as
its co-author. The maintainer takes its name and email from the pull request's author as GitHub shows that account,
not from the commits, and confirms them with you, so use an address you are happy to publish (GitHub's noreply address
works). The change reaches this repository with the next release, whose commit carries the trailer so that GitHub
credits you, and the maintainer closes the pull request with a link to that release.

Every pull request carries the statement in this repository's pull request template
(`.github/PULL_REQUEST_TEMPLATE.md`): by submitting it, you confirm that the maintainers can use, modify, copy and
redistribute your contribution under the terms of their choice.


## Release notes

`CHANGELOG.md` lists, for each release, what changed for users, and the maintainers write its notes. If your change
affects users, say in the pull request what changed for them, in one plain line, and whether it is a breaking change,
a feature or a fix:

```
Fix: a query that returns no rows no longer fails with "invalid cursor state".
```

A breaking change is anything an existing user has to change (SQL, a setting, a removed or renamed option); a feature
is something new users can do; a fix corrects behaviour. A change with no effect on users, such as a test or a
refactoring, needs no such line. If you are not sure which applies, say so in the pull request: the maintainer who
re-applies it writes the release note.


## Building from source

You need git, make, CMake 3.19 or later (3.21 or later for `windows_amd64`), a C++17 compiler and Python 3; with [Ninja](https://ninja-build.org)
installed, `GEN=ninja` makes builds faster. The extension is built against the DuckDB commit that `duckdb.commit`
pins, and `make duckdb` clones that commit into `duckdb/`; a `duckdb/` already there, a clone or a link to a DuckDB tree
of your own, must be at that commit, and `make duckdb` refuses one that is not:

```
git clone https://github.com/aws/duckdb-redshift.git
cd duckdb-redshift
make duckdb                         # DuckDB at the commit duckdb.commit pins, in duckdb/
make                                # the extension and a DuckDB shell with it, in build/release/
make test_mock                      # the SQL and C++ tests against an in-memory stand-in for Redshift
make release REDSHIFT_CLIENT=odbc   # the real client, with the embedded Amazon Redshift ODBC driver
make format-check                   # clang-format 11.0.1 exactly
```

`make clean` cleans `duckdb/` too, and so removes the `build/` of a DuckDB tree that `duckdb/` links to.

`make test_mock` needs no driver, credentials or network, and is the first suite to run for a change above the driver.
The ODBC build downloads the driver release that `third_party/redshift_odbc/driver.lock` names, with curl, and checks
its SHA-256 before embedding it; [third_party/redshift_odbc/README.md](third_party/redshift_odbc/README.md) describes
the lock and the checks.
[test/README.md](test/README.md) describes every suite, what each one needs, and which run against a live Redshift
target. Run `make format-check` and `make tidy-check` before you send a change.


## Finding contributions to work on
Looking at the existing issues is a great way to find something to contribute on. As our projects, by default, use the
default GitHub issue labels (enhancement/bug/duplicate/help wanted/invalid/question/wontfix), looking at any 'help wanted'
issues is a great place to start.


## Code of Conduct
This project has adopted the [Amazon Open Source Code of Conduct](https://aws.github.io/code-of-conduct).
For more information see the [Code of Conduct FAQ](https://aws.github.io/code-of-conduct-faq) or contact
opensource-codeofconduct@amazon.com with any additional questions or comments.


## Security issue notifications
If you discover a potential security issue in this project we ask that you notify AWS/Amazon Security via our
[vulnerability reporting page](https://aws.amazon.com/security/vulnerability-reporting/). Please do **not** create a
public github issue.


## Licensing

See the [LICENSE](LICENSE) file for our project's licensing. We will ask you to confirm the licensing of your contribution.
