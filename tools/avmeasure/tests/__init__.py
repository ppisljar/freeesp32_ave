"""Makes `python3 -m unittest discover -s tests` work.

Without this file unittest refuses the directory ("Start directory is not
importable"), and the only way to run the suite is by invoking the test file
directly -- which is easy to get wrong when the README advertises both forms.
"""
