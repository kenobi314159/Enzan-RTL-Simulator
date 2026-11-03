# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sv-tests runner for Enzan.
#
# sv-tests loads a runner by name from RUNNERS_DIR, and run-suite.sh points that here, so this
# file is used in place rather than copied into the suite. sv-tests then decides a case's result
# exactly as it does for every other tool: the tool succeeding or failing as the case expects,
# and for a simulation every `:assert:` line the design prints holding.

import os

from BaseRunner import BaseRunner


class Enzan(BaseRunner):
    def __init__(self):
        super().__init__('enzan', 'enzan', {'parsing', 'elaboration', 'simulation'})
        self.url = 'https://github.com/kenobi314159/Enzan-RTL-Simulator'

    def prepare_run_cb(self, tmp_dir, params):
        # The sources go in a file list because `elaborate` and `run` take their sources from
        # one, and their one positional word is the top level.
        with open(os.path.join(tmp_dir, 'files.f'), 'w') as listing:
            for source in params['files']:
                listing.write(source + '\n')

        command = 'run' if params['mode'] == 'simulation' else 'elaborate'
        arguments = [self.executable, command]
        for incdir in params['incdirs']:
            arguments += ['-I', incdir]
        for define in params['defines']:
            arguments += ['-D', define]
        arguments += ['-f', 'files.f']
        if params['top_module'] != '':
            arguments.append(params['top_module'])

        # Through a shell, so that a crash is an exit status of 128 and the signal: sv-tests
        # counts 126 and above as a crash, and a negative status from a signal would otherwise
        # read as an ordinary failure, which passes every case that expects one.
        self.cmd = ['sh', '-c', '"$0" "$@"'] + arguments
