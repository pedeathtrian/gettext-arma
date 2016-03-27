#### GNU gettext with support for Arma files

GNU gettext with support for Arma files is like GNU gettext without one, but only with one.
(Original GNU gettext's `README` file is still available as `README`).

Differences:
* `xgettext` takes `"arma"` as language name in `--language` option.  
  Make sure you specify `--language=arma` or `-L arma` when processing .cpp, .hpp, or other Arma files that `xgettext` might confuse with C++ or other languages files.

Plans:
* Make `msgfmt` output to Arma's `Stringtable.xml` format
* Make `msgunfmt` parse Arma's `Stringtable.xml` into multiple .po files
