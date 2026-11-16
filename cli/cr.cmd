@echo off
rem Shortcut entry for cli-resume. Keep pure ASCII: cmd parses .cmd as ANSI,
rem so UTF-8 Chinese in this file would break the following line.
node "%~dp0bin\cli-resume.js" %*
