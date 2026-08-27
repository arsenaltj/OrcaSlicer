@echo off
rem OrcaSlicer AI local test configuration template.
rem The packaging script copies this file to ai-config.bat in the release package.
rem Fill the two API key values in that copied file. Never commit real keys.

set "OPENAI_API_KEY="
set "TRIPO_API_KEY="

rem The defaults below normally do not need to be changed.
set "OPENAI_BASE_URL=https://laotie.dev"
set "OPENAI_TEXT_MODEL=gpt-5.4"
set "OPENAI_IMAGE_MODEL=gpt-image-2"
set "TRIPO_API_BASE=https://openapi.tripo3d.com/v3"
set "TRIPO_MODEL=v3.1-20260211"
set "PYTHON_EXE=%~dp0..\runtime\python\python.exe"
set "ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK=1"
