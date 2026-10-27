# Deployment Guide: LSM-Tree Key-Value Engine

## Hosting & Infrastructure
- **Current Status**: `UNDER-CONSTRUCTION`
- **Target Infrastructure**: Dedicated bare-metal or containerized cloud VPS instances with kernel bypass or raw UDP/TCP socket support.

## Containerization
A multi-stage `Dockerfile` and `docker-compose.yml` are provided in the repository root for reproducible container execution.

*Note: Free-tier PaaS environments often impose cold-starts and restrict raw socket networking; production deployments should run on dedicated Linux server environments.*
