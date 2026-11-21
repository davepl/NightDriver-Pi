# Vendored dependency

`picojson.h` is the unmodified header from [picojson v1.3.0](https://github.com/kazuho/picojson/blob/v1.3.0/picojson.h), retrieved 2026-10-30. Its BSD 2-clause license and copyright notice are retained at the start of the header. Binary distributions must reproduce that notice and license in accompanying materials. It is compatible with this project's GPL-3.0-or-later source notices.

NightDriver-Pi uses picojson's parse-context interface to reject duplicate keys and deep nesting. The configuration schema accepts only integers (no float fields), and the loader additionally rejects leading zeroes, malformed UTF-8, and trailing data.
