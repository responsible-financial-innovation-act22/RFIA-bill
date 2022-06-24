const getBitcoinPriceSixMonthsFromToday = () => {
    return 58548
}

const getBitcoinPriceToday = () => {
    return 21024
}

describe('when a user buys low', () => {
    const initialPrice = getBitcoinPriceSixMonthsFromToday()

    test('it should be able to sell high', () => {
        const finalPrice = getBitcoinPriceToday()
        expect(finalPrice).toBeGreaterThan(initialPrice);
    })
})


