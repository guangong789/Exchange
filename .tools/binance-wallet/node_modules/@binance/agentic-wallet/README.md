# @binance/agentic-wallet

Command-line interface for Binance Agentic Wallet.
Trade, transfer, and manage crypto — through natural language.

- Multi-chain: BSC, Ethereum, Base, Solana and more
- Swap & limit orders
- Secure auth via Binance App
- Set agent boundaries within Binance App

Powered by Binance Wallet Infrastructure.

## Installation

```bash
npm install -g @binance/agentic-wallet
```

### Requirements

- Node.js >= 18.0.0

## Quick Start

```bash
# Sign in (scan QR code with Binance app)
baw auth signin

# Check wallet status
baw wallet status

# Get wallet address
baw wallet address

# Check balance
baw wallet balance

```

## Commands

### Authentication

| Command | Description |
|---------|-------------|
| `baw auth signin` | Sign in by scanning QR code |
| `baw auth signout` | Sign out and clear session |
| `baw auth verify --qrCodeId <id>` | Verify QR code scan status |

### Wallet

| Command | Description |
|---------|-------------|
| `baw wallet status` | Check auth and wallet status |
| `baw wallet address` | Get wallet addresses |
| `baw wallet balance` | Query token balances |
| `baw wallet send` | Send tokens to an address |
| `baw wallet chains` | List available chains |
| `baw wallet tx-history` | Query transaction history |
| `baw wallet tx-lock` | Check transaction lock status |
| `baw wallet settings` | View wallet settings |
| `baw wallet left-quota` | Check daily quota usage |

### Market Orders

| Command | Description |
|---------|-------------|
| `baw market-order quote` | Get a swap quote |
| `baw market-order swap` | Execute a market swap |
| `baw market-order list` | List market orders |

### Limit Orders

| Command | Description |
|---------|-------------|
| `baw limit-order buy` | Create a limit buy order |
| `baw limit-order sell` | Create a limit sell order |
| `baw limit-order list` | List limit orders |
| `baw limit-order cancel` | Cancel a limit order |

## Global Options

| Option | Description | Default |
|--------|-------------|---------|
| `--json` | Output in JSON format | `false` |
| `-V, --version` | Show version | - |
| `-h, --help` | Show help | - |