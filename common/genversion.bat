@ECHO OFF
@rem OpenJOC downstream modification (openjoc-0.12.0, 2026-08-25):
@rem handle hyphenated downstream tags without generating a non-integer build macro.
SETLOCAL

PUSHD "%~dp0"

SET nbMAJOR_PART=0
SET nbCOMMIT_PART=0
SET nbHASH_PART=00000
SET OLDVER=
SET strTAG=

:: check for git presence
CALL git rev-parse --is-inside-work-tree >NUL 2>&1
IF ERRORLEVEL 1 (
    GOTO NOGIT
)

:: Count commits after the nearest tag without parsing the tag text. Downstream
:: OpenJOC tags contain hyphens, so splitting git-describe output on '-' would
:: turn a dotted tag component into an invalid integer preprocessor macro.
FOR /F "tokens=*" %%A IN ('git describe --tags --abbrev^=0 HEAD') DO (
  SET strTAG=%%A
)
IF DEFINED strTAG (
  FOR /F "tokens=*" %%A IN ('git rev-list --count "%strTAG%..HEAD"') DO SET nbCOMMIT_PART=%%A
) ELSE (
  FOR /F "tokens=*" %%A IN ('git rev-list --count HEAD') DO SET nbCOMMIT_PART=%%A
)
FOR /F "tokens=*" %%A IN ('git rev-parse --short^=5 HEAD') DO SET nbHASH_PART=%%A

:WRITE_VER

:: check if info changed, and write if needed
IF EXIST includes\version_rev.h (
    SET /P OLDVER=<includes\version_rev.h
)
SET NEWVER=#define LAV_VERSION_BUILD %nbCOMMIT_PART%
IF NOT "%NEWVER%" == "%OLDVER%" (
    :: swapped order to avoid trailing newlines
    > includes\version_rev.h ECHO %NEWVER%
)
GOTO :END

:NOGIT
echo Git not found
goto WRITE_VER

:END
POPD
ENDLOCAL
