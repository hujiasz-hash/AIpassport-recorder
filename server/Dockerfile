FROM python:3.12-alpine

# 只用标准库，无需 pip install。
WORKDIR /app
COPY server.py /app/server.py

# 非 root 运行。NAS 上把数据目录的属主改成这个 uid，或反过来改这里。
RUN adduser -D -u 1000 aipass
USER aipass

ENV DATA_DIR=/data \
    PORT=8000 \
    HOST=0.0.0.0 \
    PYTHONUNBUFFERED=1

EXPOSE 8000
CMD ["python3", "/app/server.py"]