# The System Link hub (tools/fci_hub.py) in a container. Build from the repository root:
#   docker build -f tools/hub/Dockerfile -t fci-hub .
#   docker run -d --restart unless-stopped -p 47800:47800/udp -p 127.0.0.1:47880:47880 fci-hub
# The status page is published to the server itself only; change that mapping to make it public.
FROM python:3.12-slim
COPY tools/fci_hub.py /opt/fci-hub/fci_hub.py
USER nobody
EXPOSE 47800/udp 47880/tcp
ENTRYPOINT ["python", "-B", "-u", "/opt/fci-hub/fci_hub.py", "--port", "47800", "--status-port", "47880", "--status-bind", "0.0.0.0"]
