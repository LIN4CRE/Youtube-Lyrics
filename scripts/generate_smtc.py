import os
from pathlib import Path

src = Path('src/win/smtc.ps1').read_text(encoding='utf-8')
assert ')YTLYSPLIT"' not in src
out = (
    '// Generated - do not edit.\n#pragma once\nnamespace ytl {\n'
    'static const char kSmtcScript[] = R"YTLYSPLIT(\n'
    + src + '\n)YTLYSPLIT";\n}\n'
)
Path('.build').mkdir(exist_ok=True)
Path('.build/smtc_script.h').write_text(out, encoding='utf-8')
print("generated .build/smtc_script.h")
